#include "gtest/gtest.h"

#include "pochivm/common.h"
#include "pochivm/error_context.h"
#include "pochivm/codegen_arena_allocator.h"
#include "fastinterp/fastinterp_helper.h"
#include "fastinterp/fastinterp_codegen_helper.h"
#include "fastinterp/wasm_memory_ptr.h"

#include "wasi_impl.h"

#include <unistd.h>
#include <asm/prctl.h>
#include <sys/prctl.h>
#include <sys/syscall.h>

namespace PochiVM
{

class ShallowStream
{
public:
    // Read a directly-encoded integer or floating point value
    // This function assumes that the binary is well-formatted (i.e. the module has passed validation).
    //
    template<typename T>
    T WARN_UNUSED ReadScalar()
    {
        static_assert(std::is_arithmetic<T>::value && !std::is_same<T, bool>::value, "T must be integral or floating point");
        assert(m_current + sizeof(T) <= m_end);
        T result;
        memcpy(&result, reinterpret_cast<void*>(m_current), sizeof(T));
        m_current += sizeof(T);
        return result;
    }

    template<typename T>
    T WARN_UNUSED PeekScalar()
    {
        static_assert(std::is_arithmetic<T>::value && !std::is_same<T, bool>::value, "T must be integral or floating point");
        assert(m_current + sizeof(T) <= m_end);
        T result;
        memcpy(&result, reinterpret_cast<void*>(m_current), sizeof(T));
        return result;
    }

    // Read a LEB-encoded integer value
    // This function assumes that the binary is well-formatted (i.e. the module has passed validation).
    //
    template<typename T>
    T WARN_UNUSED ReadIntLeb()
    {
        static_assert(std::is_integral<T>::value && !std::is_same<T, bool>::value, "T must be integral");
        using U = typename std::make_unsigned<T>::type;
        uint32_t shift = 0;
        U result = 0;
        while (true)
        {
            assert(shift < sizeof(T) * 8);
            uint8_t value = *reinterpret_cast<uint8_t*>(m_current);
            result |= static_cast<U>(value & 0x7f) << shift;
            shift += 7;
            m_current++;
            if ((value & 0x80) == 0)
            {
                // If the type is signed and the value is negative, do sign extension
                //
                if constexpr(std::is_signed<T>::value)
                {
                    if ((value & 0x40) && shift < sizeof(T) * 8)
                    {
                        result |= (~static_cast<U>(0)) << shift;
                    }
                }
                break;
            }
        }
        assert(m_current <= m_end);
        return static_cast<T>(result);
    }

    // Read a wasm string. The string is shallow (not copied).
    // This function assumes that the binary is well-formatted (i.e. the module has passed validation).
    //
    std::pair<uint32_t, const char*> WARN_UNUSED ReadShallowString()
    {
        uint32_t length = ReadIntLeb<uint32_t>();
        const char* s = reinterpret_cast<const char*>(m_current);
        m_current += length;
        assert(m_current <= m_end);
        return std::make_pair(length, s);
    }

#ifndef NDEBUG
    bool WARN_UNUSED HasMore() const
    {
        return m_current < m_end;
    }
#endif

    void SkipBytes(size_t numBytes)
    {
        m_current += numBytes;
        assert(m_current <= m_end);
    }

    ShallowStream GetShallowStreamFromNow(size_t length)
    {
        assert(m_current + length <= m_end);
        return ShallowStream(m_current, length);
    }

    friend class MemoryMappedFile;

private:
    ShallowStream(uintptr_t start, size_t DEBUG_ONLY(length))
        : m_current(start)
#ifndef NDEBUG
        , m_end(start + length)
#endif
    { }

    uintptr_t m_current;
#ifndef NDEBUG
    uintptr_t m_end;
#endif
};

class MemoryMappedFile : NonCopyable, NonMovable
{
public:
    MemoryMappedFile()
        : m_fd(-1), m_file(nullptr)
    { }

    ~MemoryMappedFile()
    {
        if (m_fd != -1 || m_file != nullptr)
        {
            munmap(reinterpret_cast<void*>(m_start), m_length);
            if (m_fd != -1)
            {
                close(m_fd);
                m_fd = -1;
            }
            if (m_file != nullptr)
            {
                fclose(m_file);
            }
        }
    }

    bool WARN_UNUSED IsInitalized() const { return m_fd != -1 || m_file != nullptr; }

    bool WARN_UNUSED Open(const char* file)
    {
        assert(!IsInitalized());
//#define INPUT_USE_MMAP
#ifdef INPUT_USE_MMAP
        bool success = false;
        m_fd = open(file, O_RDONLY);
        if (m_fd == -1)
        {
            int err = errno;
            REPORT_ERR("Failed to open file '%s' for mmap, error %d(%s).", file, err, strerror(err));
            return false;
        }
        Auto(
            if (!success) { close(m_fd); m_fd = -1; }
        );

        {
            struct stat s;
            int status = fstat(m_fd, &s);
            if (status != 0)
            {
                assert(status == -1);
                int err = errno;
                REPORT_ERR("Failed to fstat file '%s', error %d(%s).", file, err, strerror(err));
                return false;
            }
            m_length = static_cast<size_t>(s.st_size);
        }

        void* result = mmap(nullptr, m_length, PROT_READ, MAP_PRIVATE, m_fd, 0 /*offset*/);
        if (result == MAP_FAILED)
        {
            int err = errno;
            REPORT_ERR("Failed to mmap file '%s', error %d(%s).", file, err, strerror(err));
            return false;
        }
        assert(result != nullptr);

        m_start = reinterpret_cast<uintptr_t>(result);
        success = true;
        return true;
#else
        bool success = false;
        m_file = fopen(file, "r");
        if (m_file == nullptr)
        {
            int err = errno;
            REPORT_ERR("Failed to open file '%s' for mmap, error %d(%s).", file, err, strerror(err));
            return false;
        }

        Auto(
            if (!success) { fclose(m_file); m_file = nullptr; }
        );

        fseek(m_file, 0, SEEK_END);
        m_length = static_cast<size_t>(ftell(m_file));
        fseek(m_file, 0, SEEK_SET);

        void* result = mmap(nullptr, m_length, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
        if (result == MAP_FAILED)
        {
            ReleaseAssert(false && "Out Of Memory");
        }
        assert(result != nullptr);

        fread(result, 1, m_length, m_file);

        m_start = reinterpret_cast<uintptr_t>(result);
        success = true;
        return true;
#endif
    }

    ShallowStream GetShallowStream()
    {
        return ShallowStream(m_start, m_length);
    }

    bool WARN_UNUSED HasMore(const ShallowStream& s)
    {
        assert(m_start <= s.m_current && s.m_current <= m_start + m_length);
        assert(s.m_end == m_start + m_length);
        return s.m_current < m_start + m_length;
    }

private:
    int m_fd;
    FILE* m_file;
    uintptr_t m_start;
    size_t m_length;
};

// Order is hardcoded!
//
enum class WasmValueType : uint8_t
{
    I32,
    I64,
    F32,
    F64,
    X_END_OF_ENUM
};

struct WasmValueTypeHelper
{
    static WasmValueType ALWAYS_INLINE WARN_UNUSED Parse(ShallowStream& reader)
    {
        // valtype::= 0x7F => i32, 0x7E => i64, 0x7D => f32, 0x7C => f64
        //
        uint8_t value = reader.ReadScalar<uint8_t>();
        value ^= 0x7f;
        assert(value < 4);
        return static_cast<WasmValueType>(value);
    }

    static bool IsIntegral(WasmValueType t)
    {
        assert(t < WasmValueType::X_END_OF_ENUM);
        return t <= WasmValueType::I64;
    }

    static bool IsFloatingPoint(WasmValueType t)
    {
        return !IsIntegral(t);
    }
};

struct WasmFunctionType
{
    void ALWAYS_INLINE Parse(TempArenaAllocator& alloc, ShallowStream& reader)
    {
        // https://webassembly.github.io/spec/core/binary/types.html#binary-functype
        // Function types are encoded by the byte 0x60 followed by the respective vectors of parameter and result types.
        //
        uint8_t magic = reader.ReadScalar<uint8_t>();
        assert(magic == 0x60);
        std::ignore = magic;

        m_numParams = reader.ReadIntLeb<uint32_t>();
        m_numIntParams = 0;
        m_numFloatParams = 0;
        WasmValueType* tmp = reinterpret_cast<WasmValueType*>(alloca(sizeof(WasmValueType) * m_numParams));
        for (uint32_t i = 0; i < m_numParams; i++)
        {
            tmp[i] = WasmValueTypeHelper::Parse(reader);
            if (tmp[i] == WasmValueType::I32 || tmp[i] == WasmValueType::I64)
            {
                m_numIntParams++;
            }
            else
            {
                m_numFloatParams++;
            }
        }

        m_numReturns = reader.ReadIntLeb<uint32_t>();
        if (m_numReturns > 1)
        {
            TestAssert(false && "multiple-value extension is currently not supported");
        }
        m_types = new (std::align_val_t(1), alloc) WasmValueType[m_numParams + m_numReturns];
        memcpy(m_types, tmp, sizeof(WasmValueType) * m_numParams);
        for (uint32_t i = 0; i < m_numReturns; i++)
        {
            m_types[m_numParams + i] = WasmValueTypeHelper::Parse(reader);
        }
    }

    WasmValueType GetParamType(uint32_t i) const
    {
        assert(i < m_numParams);
        return m_types[i];
    }

    WasmValueType GetReturnType(uint32_t i) const
    {
        assert(i < m_numReturns);
        return m_types[m_numParams + i];
    }

    uint32_t m_numParams;
    uint32_t m_numReturns;
    uint32_t m_numIntParams;
    uint32_t m_numFloatParams;
    WasmValueType* m_types;
};

class WasmFunctionTypeSection
{
public:
    WasmFunctionTypeSection()
        : m_numFunctionTypes(0)
        , m_functionTypes(nullptr)
    { }

    // Parse the function types section.
    // 'reader' should be the exact range of this section.
    //
    void ParseSection(TempArenaAllocator& alloc, ShallowStream reader)
    {
        // https://webassembly.github.io/spec/core/binary/modules.html#binary-typesec
        // The type section has the id 1. It decodes into a vector of function types that represent the types component of a module.
        //
        m_numFunctionTypes = reader.ReadIntLeb<uint32_t>();
        m_functionTypes = new (alloc) WasmFunctionType[m_numFunctionTypes];
        for (uint32_t i = 0; i < m_numFunctionTypes; i++)
        {
            m_functionTypes[i].Parse(alloc, reader);
        }
        assert(!reader.HasMore());
    }

    uint32_t GetNumFunctionTypes() const
    {
        return m_numFunctionTypes;
    }

    WasmFunctionType GetFunctionTypeFromIdx(uint32_t typeIdx)
    {
        assert(typeIdx < m_numFunctionTypes);
        return m_functionTypes[typeIdx];
    }

private:
    uint32_t m_numFunctionTypes;
    WasmFunctionType* m_functionTypes;
};

struct WasmImportedEntityName
{
    uint32_t m_lv1NameLen;
    uint32_t m_lv2NameLen;
    const char* m_lv1Name;
    const char* m_lv2Name;

    void ALWAYS_INLINE Parse(ShallowStream& reader)
    {
        // https://webassembly.github.io/spec/core/binary/modules.html#binary-import
        //
        std::tie(m_lv1NameLen, m_lv1Name) = reader.ReadShallowString();
        std::tie(m_lv2NameLen, m_lv2Name) = reader.ReadShallowString();
    }
};

struct WasmTableOrMemoryLimit
{
    uint32_t m_minSize;
    uint32_t m_maxSize;

    WasmTableOrMemoryLimit()
        : m_minSize(0)
        , m_maxSize(std::numeric_limits<uint32_t>::max())
    { }

    void ALWAYS_INLINE Parse(ShallowStream& reader)
    {
        // https://webassembly.github.io/spec/core/binary/types.html#binary-limits
        //
        uint8_t kind = reader.ReadScalar<uint8_t>();
        m_minSize = reader.ReadIntLeb<uint32_t>();
        if (kind == 0)
        {
            m_maxSize = std::numeric_limits<uint32_t>::max();
        }
        else
        {
            assert(kind == 1);
            m_maxSize = reader.ReadIntLeb<uint32_t>();
            assert(m_minSize <= m_maxSize);
        }
    }
};

struct WasmGlobal
{
    void ALWAYS_INLINE Parse(ShallowStream& reader)
    {
        m_valueType = WasmValueTypeHelper::Parse(reader);
        uint8_t isMut = reader.ReadScalar<uint8_t>();
        assert(isMut == 0 || isMut == 1);
        m_isMutable = isMut;
    }

    WasmValueType m_valueType;
    bool m_isMutable;
};

class WasmImportSection
{
public:
    WasmImportSection()
        : m_numImportedFunctions(0)
        , m_numImportedGlobals(0)
        , m_totalImports(0)
        , m_isTableImported(false)
        , m_isMemoryImported(false)
    { }

    void ParseSection(TempArenaAllocator& alloc, ShallowStream reader)
    {
        // https://webassembly.github.io/spec/core/binary/modules.html#binary-importsec
        //
        uint32_t totalImports = reader.ReadIntLeb<uint32_t>();
        m_totalImports = totalImports;
        m_numImportedFunctions = 0;
        m_numImportedGlobals = 0;
        m_importNames = new (alloc) WasmImportedEntityName[totalImports + 2];
        m_importedFunctionTypes = new (alloc) uint32_t[totalImports];
        m_importedGlobalTypes = new (alloc) WasmGlobal[totalImports];
        for (uint32_t i = 0; i < totalImports; i++)
        {
            WasmImportedEntityName name;
            name.Parse(reader);
            uint8_t importType = reader.ReadScalar<uint8_t>();
            if (importType == 0)
            {
                // function type
                //
                m_importedFunctionTypes[m_numImportedFunctions] = reader.ReadIntLeb<uint32_t>();
                m_importNames[m_numImportedFunctions] = name;
                m_numImportedFunctions++;
            }
            else if (importType == 3)
            {
                // global type
                //
                m_importNames[m_totalImports - 1 - m_numImportedGlobals] = name;
                m_importedGlobalTypes[m_numImportedGlobals].Parse(reader);
                m_numImportedGlobals++;
            }
            else if (importType == 1)
            {
                // table type
                //
                assert(!m_isTableImported);
                m_isTableImported = true;
                m_importNames[m_totalImports] = name;
                uint8_t magic;
                magic = reader.ReadScalar<uint8_t>();
                assert(magic == 0x70);
                std::ignore = magic;
                m_importedTableLimit.Parse(reader);
            }
            else if (importType == 2)
            {
                // memory type
                //
                assert(!m_isMemoryImported);
                m_isMemoryImported = true;
                m_importNames[m_totalImports + 1] = name;
                m_importedMemoryLimit.Parse(reader);
            }
            else
            {
                assert(false);
            }
        }
        assert(!reader.HasMore());
    }

    bool IsTableImported() const { return m_isTableImported; }
    bool IsMemoryImported() const { return m_isMemoryImported; }
    WasmImportedEntityName GetImportedTableName() const { assert(IsTableImported()); return m_importNames[m_totalImports]; }
    WasmTableOrMemoryLimit GetImportedTableLimit() const { assert(IsTableImported()); return m_importedTableLimit; }
    WasmImportedEntityName GetImportedMemoryName() const { assert(IsMemoryImported()); return m_importNames[m_totalImports + 1]; }
    WasmTableOrMemoryLimit GetImportedMemoryLimit() const { assert(IsMemoryImported()); return m_importedMemoryLimit; }

    WasmImportedEntityName GetImportedFunctionName(uint32_t funcIdx) const
    {
        assert(funcIdx < m_numImportedFunctions);
        return m_importNames[funcIdx];
    }

    uint32_t GetImportedFunctionType(uint32_t funcIdx) const
    {
        assert(funcIdx < m_numImportedFunctions);
        return m_importedFunctionTypes[funcIdx];
    }

    WasmImportedEntityName GetImportedGlobalName(uint32_t globalIdx) const
    {
        assert(globalIdx < m_numImportedGlobals);
        return m_importNames[m_totalImports - 1 - globalIdx];
    }

    WasmGlobal GetImportedGlobalType(uint32_t globalIdx) const
    {
        assert(globalIdx < m_numImportedGlobals);
        return m_importedGlobalTypes[globalIdx];
    }

    uint32_t GetNumImportedFunctions() const { return m_numImportedFunctions; }
    uint32_t* GetImportedFunctionTypesArray() const { return m_importedFunctionTypes; }

    uint32_t GetNumImportedGlobals() const { return m_numImportedGlobals; }
    WasmGlobal* GetImportedGlobalTypesArray() const { return m_importedGlobalTypes; }

private:
    // Imports may show up in any order, but we don't want to use std::vector for dynamic-length array for performance reasons.
    // So internally, we layout the entities as
    //   [imported functions] [padding] [imported globals in reverse order] [imported table] [imported memory]
    // imported table is always at #numImports and importedMemory is always at #numImports+1
    //
    uint32_t m_numImportedFunctions;
    uint32_t m_numImportedGlobals;
    WasmImportedEntityName* m_importNames;
    uint32_t* m_importedFunctionTypes;
    WasmGlobal* m_importedGlobalTypes;
    // WASM spec atm only allows up to 1 memory/table
    //
    WasmTableOrMemoryLimit m_importedTableLimit;
    WasmTableOrMemoryLimit m_importedMemoryLimit;
    uint32_t m_totalImports;
    bool m_isTableImported;
    bool m_isMemoryImported;
};

struct WasmFunctionDeclarationSection
{
    void ParseEmptySection(WasmImportSection* imports)
    {
        m_numImportedFunctions = imports->GetNumImportedFunctions();
        m_numFunctions = m_numImportedFunctions;
        m_functionDeclarations = imports->GetImportedFunctionTypesArray();
    }

    void ParseSection(TempArenaAllocator& alloc, ShallowStream reader, WasmImportSection* imports)
    {
        uint32_t numInternalFuncs = reader.ReadIntLeb<uint32_t>();
        m_numImportedFunctions = imports->GetNumImportedFunctions();
        m_numFunctions = m_numImportedFunctions + numInternalFuncs;
        m_functionDeclarations = new (alloc) uint32_t[m_numFunctions];
        m_functionStackSize = new (alloc) uint32_t[m_numFunctions];
        m_functionEntryPoint = new (alloc) uint8_t*[m_numFunctions];
        memcpy(m_functionDeclarations, imports->GetImportedFunctionTypesArray(), sizeof(uint32_t) * m_numImportedFunctions);
        for (uint32_t i = m_numImportedFunctions; i < m_numFunctions; i++)
        {
            m_functionDeclarations[i] = reader.ReadIntLeb<uint32_t>();
        }
        assert(!reader.HasMore());
    }

    bool IsFunctionIdxImported(uint32_t functionIdx) const
    {
        assert(functionIdx < m_numFunctions);
        return functionIdx < m_numImportedFunctions;
    }

    uint32_t GetFunctionTypeIdxFromFunctionIdx(uint32_t functionIdx) const
    {
        assert(functionIdx < m_numFunctions);
        return m_functionDeclarations[functionIdx];
    }

    uint32_t m_numFunctions;
    uint32_t m_numImportedFunctions;
    // m_functionDeclarations[i] is the function type index of function i
    // [0, m_numImportedFunctions) are imported functions
    //
    uint32_t* m_functionDeclarations;
    uint32_t* m_functionStackSize;
    uint8_t** m_functionEntryPoint;
};

struct WasmTableSection
{
    WasmTableSection()
        : m_hasTable(false)
    { }

    void ParseSection(ShallowStream reader)
    {
        uint32_t numTables = reader.ReadIntLeb<uint32_t>();
        // current WASM spec allows up to 1 table.
        //
        assert(numTables <= 1);
        if (numTables == 1)
        {
            m_hasTable = true;
            uint8_t magic;
            magic = reader.ReadScalar<uint8_t>();
            assert(magic == 0x70);
            std::ignore = magic;
            m_limit.Parse(reader);
            assert(m_limit.m_minSize == m_limit.m_maxSize);
        }
        assert(!reader.HasMore());
    }

    WasmTableOrMemoryLimit m_limit;
    bool m_hasTable;
};

struct WasmMemorySection
{
    WasmMemorySection()
        : m_hasMemory(false)
    { }

    void ParseSection(ShallowStream reader)
    {
        uint32_t numMemories = reader.ReadIntLeb<uint32_t>();
        // current WASM spec allows up to 1 memory
        //
        assert(numMemories <= 1);
        if (numMemories == 1)
        {
            m_hasMemory = true;
            m_limit.Parse(reader);
        }
        assert(!reader.HasMore());
    }

    WasmTableOrMemoryLimit m_limit;
    bool m_hasMemory;
};

class WasmConstantExpression
{
public:
    void ALWAYS_INLINE Parse(ShallowStream& reader
#ifndef NDEBUG
                           , WasmValueType valueType
                           , uint32_t globalLimit
#endif
                             )
    {
        uint8_t opcode = reader.ReadScalar<uint8_t>();
        if (opcode == 0x23)
        {
            // global.get
            //
            m_isInitByGlobal = true;
            m_globalIdx = reader.ReadIntLeb<uint32_t>();
            assert(m_globalIdx < globalLimit);
        }
        else
        {
            // must be a 't.const' matching expected type
            //
            assert(opcode == 0x41 + static_cast<uint8_t>(valueType));
            WasmValueType globalType = static_cast<WasmValueType>(opcode - 0x41);
            m_isInitByGlobal = false;
            // For integers, the operand is encoded as *signed* integers
            //
            if (globalType == WasmValueType::I32)
            {
                int32_t value = reader.ReadIntLeb<int32_t>();
                memcpy(m_initRawBytes, &value, 4);
            }
            else if (globalType == WasmValueType::I64)
            {
                int64_t value = reader.ReadIntLeb<int64_t>();
                memcpy(m_initRawBytes, &value, 8);
            }
            else if (globalType == WasmValueType::F32)
            {
                static_assert(sizeof(float) == 4);
                float value = reader.ReadScalar<float>();
                memcpy(m_initRawBytes, &value, 4);
            }
            else
            {
                assert(globalType == WasmValueType::F64);
                static_assert(sizeof(double) == 8);
                double value = reader.ReadScalar<double>();
                memcpy(m_initRawBytes, &value, 8);
            }
        }
        // The next opcode must be an 'end' opcode
        //
        {
            uint8_t endOpcode = reader.ReadScalar<uint8_t>();
            assert(endOpcode == 0x0B);
            std::ignore = endOpcode;
        }
    }

    // https://webassembly.github.io/spec/core/valid/instructions.html#valid-constant
    // A WASM constant expression must be either a 't.const c' or a 'global.get x'
    //
    // Whether this constant is initialized by a global
    //
    bool m_isInitByGlobal;
    // If yes, the idx of the global
    //
    uint32_t m_globalIdx;
    // Otherwise, the constant bytes to initialize this value
    //
    char m_initRawBytes[8];
};

struct WasmGlobalSection
{
    void ParseEmptySection(WasmImportSection* imports)
    {
        m_numImportedGlobals = imports->GetNumImportedGlobals();
        m_numGlobals = m_numImportedGlobals;
        m_globals = imports->GetImportedGlobalTypesArray();
        m_initExprs = nullptr;
    }

    void ParseSection(TempArenaAllocator& alloc, ShallowStream reader, WasmImportSection* imports)
    {
        uint32_t numInternalGlobals = reader.ReadIntLeb<uint32_t>();
        m_numImportedGlobals = imports->GetNumImportedGlobals();
        m_numGlobals = m_numImportedGlobals + numInternalGlobals;
        m_globals = new (alloc) WasmGlobal[m_numGlobals];
        memcpy(m_globals, imports->GetImportedGlobalTypesArray(), sizeof(WasmGlobal) * m_numImportedGlobals);
        m_initExprs = new (alloc) WasmConstantExpression[numInternalGlobals];
        for (uint32_t i = 0; i < numInternalGlobals; i++)
        {
            m_globals[m_numImportedGlobals + i].Parse(reader);
            // https://webassembly.github.io/spec/core/valid/instructions.html#expressions
            // Currently, constant expressions occurring as initializers of globals are further constrained
            // in that contained global.get instructions are only allowed to refer to imported globals.
            //
            m_initExprs[i].Parse(reader
#ifndef NDEBUG
                               , m_globals[m_numImportedGlobals + i].m_valueType
                               , m_numImportedGlobals /*globalLimit*/
#endif
            );
        }
        assert(!reader.HasMore());
    }

    uint32_t m_numGlobals;
    uint32_t m_numImportedGlobals;
    WasmGlobal* m_globals;
    // init expressions for each non-imported global
    //
    WasmConstantExpression* m_initExprs;
};

struct WasmExportedEntity
{
    uint32_t m_entityIdx;
    uint32_t m_length;
    const char* m_name;
};

struct WasmExportSection
{
    WasmExportSection()
        : m_numFunctionsExported(0)
        , m_numGlobalsExported(0)
        , m_exportedFunctions(nullptr)
        , m_exportedGlobals(nullptr)
        , m_exportedTable(nullptr)
        , m_exportedMemory(nullptr)
    { }

    void ParseSection(TempArenaAllocator& alloc, ShallowStream reader)
    {
        uint32_t totalExports = reader.ReadIntLeb<uint32_t>();
        m_exportedFunctions = new (alloc) WasmExportedEntity[totalExports];
        m_exportedFunctionAddresses = new (alloc) uint8_t*[totalExports];
        m_exportedGlobals = new (alloc) WasmExportedEntity[totalExports];
        for (uint32_t i = 0; i < totalExports; i++)
        {
            WasmExportedEntity entity;
            std::tie(entity.m_length, entity.m_name) = reader.ReadShallowString();
            uint8_t exportType = reader.ReadScalar<uint8_t>();
            entity.m_entityIdx = reader.ReadIntLeb<uint32_t>();
            if (exportType == 0)
            {
                // function export
                //
                m_exportedFunctions[m_numFunctionsExported] = entity;
                m_numFunctionsExported++;
            }
            else if (exportType == 3)
            {
                // global export
                //
                m_exportedGlobals[m_numGlobalsExported] = entity;
                m_numGlobalsExported++;
            }
            else if (exportType == 1)
            {
                // table export
                //
                assert(entity.m_entityIdx == 0 && m_exportedTable == nullptr);
                m_exportedTable = new (alloc) WasmExportedEntity;
                *m_exportedTable = entity;
            }
            else
            {
                assert(exportType == 2);
                // memory export
                //
                assert(entity.m_entityIdx == 0 && m_exportedMemory == nullptr);
                m_exportedMemory = new (alloc) WasmExportedEntity;
                *m_exportedMemory = entity;
            }
        }
        assert(!reader.HasMore());
    }

    bool IsTableExported() const { return m_exportedTable != nullptr; }
    bool IsMemoryExported() const { return m_exportedMemory != nullptr; }
    WasmExportedEntity GetExportedTable() const { assert(IsTableExported()); return *m_exportedTable; }
    WasmExportedEntity GetExportedMemory() const { assert(IsMemoryExported()); return *m_exportedMemory; }

    uint32_t m_numFunctionsExported;
    uint32_t m_numGlobalsExported;
    WasmExportedEntity* m_exportedFunctions;
    uint8_t** m_exportedFunctionAddresses;
    WasmExportedEntity* m_exportedGlobals;
    WasmExportedEntity* m_exportedTable;
    WasmExportedEntity* m_exportedMemory;
};

struct WasmStartSection
{
    WasmStartSection()
        : m_hasStartFunction(false)
    { }

    void ParseSection(ShallowStream reader)
    {
        m_hasStartFunction = true;
        m_startFunctionIdx = reader.ReadIntLeb<uint32_t>();
        assert(!reader.HasMore());
    }

    bool m_hasStartFunction;
    uint32_t m_startFunctionIdx;
};

struct WasmElementRecord
{
    void Parse(TempArenaAllocator& alloc, ShallowStream& reader)
    {
        uint32_t tableIdx = reader.ReadIntLeb<uint32_t>();
        assert(tableIdx == 0);
        std::ignore = tableIdx;

        m_offset.Parse(reader
#ifndef NDEBUG
                     , WasmValueType::I32 /*valueType*/
                     , static_cast<uint32_t>(-1) /*globalLimit*/
#endif
        );

        uint32_t length = reader.ReadIntLeb<uint32_t>();
        m_length = length;
        m_contents = new (alloc) uint32_t[length];
        for (uint32_t i = 0; i < length; i++)
        {
            m_contents[i] = reader.ReadIntLeb<uint32_t>();
        }
    }

    WasmConstantExpression m_offset;
    uint32_t m_length;
    uint32_t* m_contents;
};

struct WasmElementSection
{
    WasmElementSection()
        : m_numRecords(0)
    { }

    void ParseSection(TempArenaAllocator& alloc, ShallowStream reader)
    {
        m_numRecords = reader.ReadIntLeb<uint32_t>();
        m_records = new (alloc) WasmElementRecord[m_numRecords];
        for (uint32_t i = 0; i < m_numRecords; i++)
        {
            m_records[i].Parse(alloc, reader);
        }
        assert(!reader.HasMore());
    }

    uint32_t m_numRecords;
    WasmElementRecord* m_records;
};

struct WasmDataRecord
{
    void Parse(ShallowStream& reader)
    {
        uint32_t memoryIdx = reader.ReadIntLeb<uint32_t>();
        assert(memoryIdx == 0);
        std::ignore = memoryIdx;

        m_offset.Parse(reader
#ifndef NDEBUG
                     , WasmValueType::I32 /*valueType*/
                     , static_cast<uint32_t>(-1) /*globalLimit*/
#endif
        );
        std::tie(m_length, m_contents) = reader.ReadShallowString();
    }

    WasmConstantExpression m_offset;
    uint32_t m_length;
    const char* m_contents;
};

enum class WasmSectionId
{
    CUSTOM_SECTION = 0,
    TYPE_SECTION = 1,
    IMPORT_SECTION = 2,
    FUNCTION_SECTION = 3,
    TABLE_SECTION = 4,
    MEMORY_SECTION = 5,
    GLOBAL_SECTION = 6,
    EXPORT_SECTION = 7,
    START_SECTION = 8,
    ELEMENT_SECTION = 9,
    CODE_SECTION = 10,
    DATA_SECTION = 11,
    X_END_OF_ENUM = 12
};

enum class WasmOpcodeOperandKind : uint8_t
{
    NONE,           // has no operands
    U32,            // one u32
    MEM_U32_U32,    // two u32, but only second operand is useful
    CONST,          // t.const
    BLOCKTYPE,      // one s33
    SPECIAL
};

struct alignas(8) WasmOpcodeInfo
{
    constexpr WasmOpcodeInfo()
        : m_isValid(false), m_isSpecial(false), m_numIntConsumes(0)
        , m_numFloatConsumes(0), m_hasOutput(false), m_isOutputIntegral(false)
        , m_outputType(WasmValueType::I32), m_operandKind(WasmOpcodeOperandKind::NONE)
    { }

    constexpr WasmOpcodeInfo(bool isValid, bool isSpecial, uint8_t numIntConsumes,
                             uint8_t numFloatConsumes, bool hasOutput, bool isOutputIntegral,
                             WasmValueType outputType, WasmOpcodeOperandKind operandKind)
        : m_isValid(isValid), m_isSpecial(isSpecial), m_numIntConsumes(numIntConsumes)
        , m_numFloatConsumes(numFloatConsumes), m_hasOutput(hasOutput), m_isOutputIntegral(isOutputIntegral)
        , m_outputType(outputType), m_operandKind(operandKind)
    { }

    bool m_isValid;

    // Is it a opcode that requires some kind of special handling?
    // fields below (excpet m_operandKind) are only useful when m_isSpecial == false
    //
    bool m_isSpecial;

    // How many stack operands does it consume?
    //
    uint8_t m_numIntConsumes;
    uint8_t m_numFloatConsumes;

    // What kind of output does it produce?
    //
    bool m_hasOutput;
    bool m_isOutputIntegral;
    WasmValueType m_outputType;

    WasmOpcodeOperandKind m_operandKind;

    static constexpr WasmOpcodeInfo Create(bool DEBUG_ONLY(isSpecial), WasmOpcodeOperandKind operandKind)
    {
        assert(isSpecial);
        return WasmOpcodeInfo { true, true, 0, 0, false, false, WasmValueType::X_END_OF_ENUM, operandKind };
    }

    static constexpr WasmOpcodeInfo Create(bool DEBUG_ONLY(isSpecial),
                                           uint8_t numIntConsume,
                                           uint8_t numFloatConsume,
                                           WasmValueType outputType,
                                           WasmOpcodeOperandKind operandKind)
    {
        assert(!isSpecial);
        return WasmOpcodeInfo {
                true /*isValue*/,
                false /*isSpecial*/,
                numIntConsume,
                numFloatConsume,
                outputType != WasmValueType::X_END_OF_ENUM,
                outputType == WasmValueType::I32 || outputType == WasmValueType::I64,
                outputType,
                operandKind
        };
    }
};

static_assert(sizeof(WasmOpcodeInfo) == 8, "unexpected size");

#define FOR_EACH_WASM_OPCODE                                                                                                \
  /*   Name          Encoding       IsSpecial   #Int Consume/#Float Consume/Output           Operand Encoding Type */       \
F( UNREACHABLE,        0x00,           true                                        ,   WasmOpcodeOperandKind::NONE        ) \
F( NOP,                0x01,           true                                        ,   WasmOpcodeOperandKind::NONE        ) \
F( BLOCK,              0x02,           true                                        ,   WasmOpcodeOperandKind::BLOCKTYPE   ) \
F( LOOP,               0x03,           true                                        ,   WasmOpcodeOperandKind::BLOCKTYPE   ) \
F( IF,                 0x04,           true                                        ,   WasmOpcodeOperandKind::BLOCKTYPE   ) \
F( ELSE,               0x05,           true                                        ,   WasmOpcodeOperandKind::NONE        ) \
F( END,                0x0B,           true                                        ,   WasmOpcodeOperandKind::NONE        ) \
F( BR,                 0x0C,           true                                        ,   WasmOpcodeOperandKind::U32         ) \
F( BR_IF,              0x0D,           true                                        ,   WasmOpcodeOperandKind::U32         ) \
F( BR_TABLE,           0x0E,           true                                        ,   WasmOpcodeOperandKind::SPECIAL     ) \
F( RETURN,             0x0F,           true                                        ,   WasmOpcodeOperandKind::NONE        ) \
                                                                                                                            \
F( CALL,               0x10,           true                                        ,   WasmOpcodeOperandKind::U32         ) \
F( CALL_INDIRECT,      0x11,           true                                        ,   WasmOpcodeOperandKind::SPECIAL     ) \
                                                                                                                            \
F( DROP,               0x1A,           true                                        ,   WasmOpcodeOperandKind::NONE        ) \
F( SELECT,             0x1B,           true                                        ,   WasmOpcodeOperandKind::NONE        ) \
                                                                                                                            \
F( LOCAL_GET,          0x20,           true                                        ,   WasmOpcodeOperandKind::U32         ) \
F( LOCAL_SET,          0x21,           true                                        ,   WasmOpcodeOperandKind::U32         ) \
F( LOCAL_TEE,          0x22,           true                                        ,   WasmOpcodeOperandKind::U32         ) \
F( GLOBAL_GET,         0x23,           true                                        ,   WasmOpcodeOperandKind::U32         ) \
F( GLOBAL_SET,         0x24,           true                                        ,   WasmOpcodeOperandKind::U32         ) \
                                                                                                                            \
F( I32_LOAD,           0x28,         false,    1,  0,  WasmValueType::I32          ,   WasmOpcodeOperandKind::MEM_U32_U32 ) \
F( I64_LOAD,           0x29,         false,    1,  0,  WasmValueType::I64          ,   WasmOpcodeOperandKind::MEM_U32_U32 ) \
F( F32_LOAD,           0x2A,         false,    1,  0,  WasmValueType::F32          ,   WasmOpcodeOperandKind::MEM_U32_U32 ) \
F( F64_LOAD,           0x2B,         false,    1,  0,  WasmValueType::F64          ,   WasmOpcodeOperandKind::MEM_U32_U32 ) \
                                                                                                                            \
F( I32_LOAD_8S,        0x2C,         false,    1,  0,  WasmValueType::I32          ,   WasmOpcodeOperandKind::MEM_U32_U32 ) \
F( I32_LOAD_8U,        0x2D,         false,    1,  0,  WasmValueType::I32          ,   WasmOpcodeOperandKind::MEM_U32_U32 ) \
F( I32_LOAD_16S,       0x2E,         false,    1,  0,  WasmValueType::I32          ,   WasmOpcodeOperandKind::MEM_U32_U32 ) \
F( I32_LOAD_16U,       0x2F,         false,    1,  0,  WasmValueType::I32          ,   WasmOpcodeOperandKind::MEM_U32_U32 ) \
                                                                                                                            \
F( I64_LOAD_8S,        0x30,         false,    1,  0,  WasmValueType::I64          ,   WasmOpcodeOperandKind::MEM_U32_U32 ) \
F( I64_LOAD_8U,        0x31,         false,    1,  0,  WasmValueType::I64          ,   WasmOpcodeOperandKind::MEM_U32_U32 ) \
F( I64_LOAD_16S,       0x32,         false,    1,  0,  WasmValueType::I64          ,   WasmOpcodeOperandKind::MEM_U32_U32 ) \
F( I64_LOAD_16U,       0x33,         false,    1,  0,  WasmValueType::I64          ,   WasmOpcodeOperandKind::MEM_U32_U32 ) \
F( I64_LOAD_32S,       0x34,         false,    1,  0,  WasmValueType::I64          ,   WasmOpcodeOperandKind::MEM_U32_U32 ) \
F( I64_LOAD_32U,       0x35,         false,    1,  0,  WasmValueType::I64          ,   WasmOpcodeOperandKind::MEM_U32_U32 ) \
                                                                                                                            \
F( I32_STORE,          0x36,         false,    2,  0,  WasmValueType::X_END_OF_ENUM,   WasmOpcodeOperandKind::MEM_U32_U32 ) \
F( I64_STORE,          0x37,         false,    2,  0,  WasmValueType::X_END_OF_ENUM,   WasmOpcodeOperandKind::MEM_U32_U32 ) \
F( F32_STORE,          0x38,         false,    1,  1,  WasmValueType::X_END_OF_ENUM,   WasmOpcodeOperandKind::MEM_U32_U32 ) \
F( F64_STORE,          0x39,         false,    1,  1,  WasmValueType::X_END_OF_ENUM,   WasmOpcodeOperandKind::MEM_U32_U32 ) \
F( I32_STORE_8,        0x3A,         false,    2,  0,  WasmValueType::X_END_OF_ENUM,   WasmOpcodeOperandKind::MEM_U32_U32 ) \
F( I32_STORE_16,       0x3B,         false,    2,  0,  WasmValueType::X_END_OF_ENUM,   WasmOpcodeOperandKind::MEM_U32_U32 ) \
F( I64_STORE_8,        0x3C,         false,    2,  0,  WasmValueType::X_END_OF_ENUM,   WasmOpcodeOperandKind::MEM_U32_U32 ) \
F( I64_STORE_16,       0x3D,         false,    2,  0,  WasmValueType::X_END_OF_ENUM,   WasmOpcodeOperandKind::MEM_U32_U32 ) \
F( I64_STORE_32,       0x3E,         false,    2,  0,  WasmValueType::X_END_OF_ENUM,   WasmOpcodeOperandKind::MEM_U32_U32 ) \
                                                                                                                            \
F( MEMORY_SIZE,        0x3F,           true                                        ,   WasmOpcodeOperandKind::U32         ) \
F( MEMORY_GROW,        0x40,           true                                        ,   WasmOpcodeOperandKind::U32         ) \
                                                                                                                            \
F( I32_CONST,          0x41,         false,    0,  0,  WasmValueType::I32          ,   WasmOpcodeOperandKind::CONST       ) \
F( I64_CONST,          0x42,         false,    0,  0,  WasmValueType::I64          ,   WasmOpcodeOperandKind::CONST       ) \
F( F32_CONST,          0x43,         false,    0,  0,  WasmValueType::F32          ,   WasmOpcodeOperandKind::CONST       ) \
F( F64_CONST,          0x44,         false,    0,  0,  WasmValueType::F64          ,   WasmOpcodeOperandKind::CONST       ) \
                                                                                                                            \
F( I32_EQZ,            0x45,         false,    1,  0,  WasmValueType::I32          ,   WasmOpcodeOperandKind::NONE        ) \
                                                                                                                            \
F( I32_EQ,             0x46,         false,    2,  0,  WasmValueType::I32          ,   WasmOpcodeOperandKind::NONE        ) \
F( I32_NE,             0x47,         false,    2,  0,  WasmValueType::I32          ,   WasmOpcodeOperandKind::NONE        ) \
F( I32_LT_S,           0x48,         false,    2,  0,  WasmValueType::I32          ,   WasmOpcodeOperandKind::NONE        ) \
F( I32_LT_U,           0x49,         false,    2,  0,  WasmValueType::I32          ,   WasmOpcodeOperandKind::NONE        ) \
F( I32_GT_S,           0x4A,         false,    2,  0,  WasmValueType::I32          ,   WasmOpcodeOperandKind::NONE        ) \
F( I32_GT_U,           0x4B,         false,    2,  0,  WasmValueType::I32          ,   WasmOpcodeOperandKind::NONE        ) \
F( I32_LE_S,           0x4C,         false,    2,  0,  WasmValueType::I32          ,   WasmOpcodeOperandKind::NONE        ) \
F( I32_LE_U,           0x4D,         false,    2,  0,  WasmValueType::I32          ,   WasmOpcodeOperandKind::NONE        ) \
F( I32_GE_S,           0x4E,         false,    2,  0,  WasmValueType::I32          ,   WasmOpcodeOperandKind::NONE        ) \
F( I32_GE_U,           0x4F,         false,    2,  0,  WasmValueType::I32          ,   WasmOpcodeOperandKind::NONE        ) \
                                                                                                                            \
F( I64_EQZ,            0x50,         false,    1,  0,  WasmValueType::I32          ,   WasmOpcodeOperandKind::NONE        ) \
                                                                                                                            \
F( I64_EQ,             0x51,         false,    2,  0,  WasmValueType::I32          ,   WasmOpcodeOperandKind::NONE        ) \
F( I64_NE,             0x52,         false,    2,  0,  WasmValueType::I32          ,   WasmOpcodeOperandKind::NONE        ) \
F( I64_LT_S,           0x53,         false,    2,  0,  WasmValueType::I32          ,   WasmOpcodeOperandKind::NONE        ) \
F( I64_LT_U,           0x54,         false,    2,  0,  WasmValueType::I32          ,   WasmOpcodeOperandKind::NONE        ) \
F( I64_GT_S,           0x55,         false,    2,  0,  WasmValueType::I32          ,   WasmOpcodeOperandKind::NONE        ) \
F( I64_GT_U,           0x56,         false,    2,  0,  WasmValueType::I32          ,   WasmOpcodeOperandKind::NONE        ) \
F( I64_LE_S,           0x57,         false,    2,  0,  WasmValueType::I32          ,   WasmOpcodeOperandKind::NONE        ) \
F( I64_LE_U,           0x58,         false,    2,  0,  WasmValueType::I32          ,   WasmOpcodeOperandKind::NONE        ) \
F( I64_GE_S,           0x59,         false,    2,  0,  WasmValueType::I32          ,   WasmOpcodeOperandKind::NONE        ) \
F( I64_GE_U,           0x5A,         false,    2,  0,  WasmValueType::I32          ,   WasmOpcodeOperandKind::NONE        ) \
                                                                                                                            \
F( F32_EQ,             0x5B,         false,    0,  2,  WasmValueType::I32          ,   WasmOpcodeOperandKind::NONE        ) \
F( F32_NE,             0x5C,         false,    0,  2,  WasmValueType::I32          ,   WasmOpcodeOperandKind::NONE        ) \
F( F32_LT,             0x5D,         false,    0,  2,  WasmValueType::I32          ,   WasmOpcodeOperandKind::NONE        ) \
F( F32_GT,             0x5E,         false,    0,  2,  WasmValueType::I32          ,   WasmOpcodeOperandKind::NONE        ) \
F( F32_LE,             0x5F,         false,    0,  2,  WasmValueType::I32          ,   WasmOpcodeOperandKind::NONE        ) \
F( F32_GE,             0x60,         false,    0,  2,  WasmValueType::I32          ,   WasmOpcodeOperandKind::NONE        ) \
                                                                                                                            \
F( F64_EQ,             0x61,         false,    0,  2,  WasmValueType::I32          ,   WasmOpcodeOperandKind::NONE        ) \
F( F64_NE,             0x62,         false,    0,  2,  WasmValueType::I32          ,   WasmOpcodeOperandKind::NONE        ) \
F( F64_LT,             0x63,         false,    0,  2,  WasmValueType::I32          ,   WasmOpcodeOperandKind::NONE        ) \
F( F64_GT,             0x64,         false,    0,  2,  WasmValueType::I32          ,   WasmOpcodeOperandKind::NONE        ) \
F( F64_LE,             0x65,         false,    0,  2,  WasmValueType::I32          ,   WasmOpcodeOperandKind::NONE        ) \
F( F64_GE,             0x66,         false,    0,  2,  WasmValueType::I32          ,   WasmOpcodeOperandKind::NONE        ) \
                                                                                                                            \
F( I32_CLZ,            0x67,         false,    1,  0,  WasmValueType::I32          ,   WasmOpcodeOperandKind::NONE        ) \
F( I32_CTZ,            0x68,         false,    1,  0,  WasmValueType::I32          ,   WasmOpcodeOperandKind::NONE        ) \
F( I32_POPCNT,         0x69,         false,    1,  0,  WasmValueType::I32          ,   WasmOpcodeOperandKind::NONE        ) \
                                                                                                                            \
F( I32_ADD,            0x6A,         false,    2,  0,  WasmValueType::I32          ,   WasmOpcodeOperandKind::NONE        ) \
F( I32_SUB,            0x6B,         false,    2,  0,  WasmValueType::I32          ,   WasmOpcodeOperandKind::NONE        ) \
F( I32_MUL,            0x6C,         false,    2,  0,  WasmValueType::I32          ,   WasmOpcodeOperandKind::NONE        ) \
F( I32_DIV_S,          0x6D,         false,    2,  0,  WasmValueType::I32          ,   WasmOpcodeOperandKind::NONE        ) \
F( I32_DIV_U,          0x6E,         false,    2,  0,  WasmValueType::I32          ,   WasmOpcodeOperandKind::NONE        ) \
F( I32_REM_S,          0x6F,         false,    2,  0,  WasmValueType::I32          ,   WasmOpcodeOperandKind::NONE        ) \
F( I32_REM_U,          0x70,         false,    2,  0,  WasmValueType::I32          ,   WasmOpcodeOperandKind::NONE        ) \
F( I32_AND,            0x71,         false,    2,  0,  WasmValueType::I32          ,   WasmOpcodeOperandKind::NONE        ) \
F( I32_OR,             0x72,         false,    2,  0,  WasmValueType::I32          ,   WasmOpcodeOperandKind::NONE        ) \
F( I32_XOR,            0x73,         false,    2,  0,  WasmValueType::I32          ,   WasmOpcodeOperandKind::NONE        ) \
F( I32_SHL,            0x74,         false,    2,  0,  WasmValueType::I32          ,   WasmOpcodeOperandKind::NONE        ) \
F( I32_SHR_S,          0x75,         false,    2,  0,  WasmValueType::I32          ,   WasmOpcodeOperandKind::NONE        ) \
F( I32_SHR_U,          0x76,         false,    2,  0,  WasmValueType::I32          ,   WasmOpcodeOperandKind::NONE        ) \
F( I32_ROTL,           0x77,         false,    2,  0,  WasmValueType::I32          ,   WasmOpcodeOperandKind::NONE        ) \
F( I32_ROTR,           0x78,         false,    2,  0,  WasmValueType::I32          ,   WasmOpcodeOperandKind::NONE        ) \
                                                                                                                            \
F( I64_CLZ,            0x79,         false,    1,  0,  WasmValueType::I64          ,   WasmOpcodeOperandKind::NONE        ) \
F( I64_CTZ,            0x7A,         false,    1,  0,  WasmValueType::I64          ,   WasmOpcodeOperandKind::NONE        ) \
F( I64_POPCNT,         0x7B,         false,    1,  0,  WasmValueType::I64          ,   WasmOpcodeOperandKind::NONE        ) \
                                                                                                                            \
F( I64_ADD,            0x7C,         false,    2,  0,  WasmValueType::I64          ,   WasmOpcodeOperandKind::NONE        ) \
F( I64_SUB,            0x7D,         false,    2,  0,  WasmValueType::I64          ,   WasmOpcodeOperandKind::NONE        ) \
F( I64_MUL,            0x7E,         false,    2,  0,  WasmValueType::I64          ,   WasmOpcodeOperandKind::NONE        ) \
F( I64_DIV_S,          0x7F,         false,    2,  0,  WasmValueType::I64          ,   WasmOpcodeOperandKind::NONE        ) \
F( I64_DIV_U,          0x80,         false,    2,  0,  WasmValueType::I64          ,   WasmOpcodeOperandKind::NONE        ) \
F( I64_REM_S,          0x81,         false,    2,  0,  WasmValueType::I64          ,   WasmOpcodeOperandKind::NONE        ) \
F( I64_REM_U,          0x82,         false,    2,  0,  WasmValueType::I64          ,   WasmOpcodeOperandKind::NONE        ) \
F( I64_AND,            0x83,         false,    2,  0,  WasmValueType::I64          ,   WasmOpcodeOperandKind::NONE        ) \
F( I64_OR,             0x84,         false,    2,  0,  WasmValueType::I64          ,   WasmOpcodeOperandKind::NONE        ) \
F( I64_XOR,            0x85,         false,    2,  0,  WasmValueType::I64          ,   WasmOpcodeOperandKind::NONE        ) \
F( I64_SHL,            0x86,         false,    2,  0,  WasmValueType::I64          ,   WasmOpcodeOperandKind::NONE        ) \
F( I64_SHR_S,          0x87,         false,    2,  0,  WasmValueType::I64          ,   WasmOpcodeOperandKind::NONE        ) \
F( I64_SHR_U,          0x88,         false,    2,  0,  WasmValueType::I64          ,   WasmOpcodeOperandKind::NONE        ) \
F( I64_ROTL,           0x89,         false,    2,  0,  WasmValueType::I64          ,   WasmOpcodeOperandKind::NONE        ) \
F( I64_ROTR,           0x8A,         false,    2,  0,  WasmValueType::I64          ,   WasmOpcodeOperandKind::NONE        ) \
                                                                                                                            \
F( F32_ABS,            0x8B,         false,    0,  1,  WasmValueType::F32          ,   WasmOpcodeOperandKind::NONE        ) \
F( F32_NEG,            0x8C,         false,    0,  1,  WasmValueType::F32          ,   WasmOpcodeOperandKind::NONE        ) \
F( F32_CEIL,           0x8D,         false,    0,  1,  WasmValueType::F32          ,   WasmOpcodeOperandKind::NONE        ) \
F( F32_FLOOR,          0x8E,         false,    0,  1,  WasmValueType::F32          ,   WasmOpcodeOperandKind::NONE        ) \
F( F32_TRUNC,          0x8F,         false,    0,  1,  WasmValueType::F32          ,   WasmOpcodeOperandKind::NONE        ) \
F( F32_NEAREST,        0x90,         false,    0,  1,  WasmValueType::F32          ,   WasmOpcodeOperandKind::NONE        ) \
F( F32_SQRT,           0x91,         false,    0,  1,  WasmValueType::F32          ,   WasmOpcodeOperandKind::NONE        ) \
                                                                                                                            \
F( F32_ADD,            0x92,         false,    0,  2,  WasmValueType::F32          ,   WasmOpcodeOperandKind::NONE        ) \
F( F32_SUB,            0x93,         false,    0,  2,  WasmValueType::F32          ,   WasmOpcodeOperandKind::NONE        ) \
F( F32_MUL,            0x94,         false,    0,  2,  WasmValueType::F32          ,   WasmOpcodeOperandKind::NONE        ) \
F( F32_DIV,            0x95,         false,    0,  2,  WasmValueType::F32          ,   WasmOpcodeOperandKind::NONE        ) \
F( F32_MIN,            0x96,         false,    0,  2,  WasmValueType::F32          ,   WasmOpcodeOperandKind::NONE        ) \
F( F32_MAX,            0x97,         false,    0,  2,  WasmValueType::F32          ,   WasmOpcodeOperandKind::NONE        ) \
F( F32_COPYSIGN,       0x98,         false,    0,  2,  WasmValueType::F32          ,   WasmOpcodeOperandKind::NONE        ) \
                                                                                                                            \
F( F64_ABS,            0x99,         false,    0,  1,  WasmValueType::F64          ,   WasmOpcodeOperandKind::NONE        ) \
F( F64_NEG,            0x9A,         false,    0,  1,  WasmValueType::F64          ,   WasmOpcodeOperandKind::NONE        ) \
F( F64_CEIL,           0x9B,         false,    0,  1,  WasmValueType::F64          ,   WasmOpcodeOperandKind::NONE        ) \
F( F64_FLOOR,          0x9C,         false,    0,  1,  WasmValueType::F64          ,   WasmOpcodeOperandKind::NONE        ) \
F( F64_TRUNC,          0x9D,         false,    0,  1,  WasmValueType::F64          ,   WasmOpcodeOperandKind::NONE        ) \
F( F64_NEAREST,        0x9E,         false,    0,  1,  WasmValueType::F64          ,   WasmOpcodeOperandKind::NONE        ) \
F( F64_SQRT,           0x9F,         false,    0,  1,  WasmValueType::F64          ,   WasmOpcodeOperandKind::NONE        ) \
                                                                                                                            \
F( F64_ADD,            0xA0,         false,    0,  2,  WasmValueType::F64          ,   WasmOpcodeOperandKind::NONE        ) \
F( F64_SUB,            0xA1,         false,    0,  2,  WasmValueType::F64          ,   WasmOpcodeOperandKind::NONE        ) \
F( F64_MUL,            0xA2,         false,    0,  2,  WasmValueType::F64          ,   WasmOpcodeOperandKind::NONE        ) \
F( F64_DIV,            0xA3,         false,    0,  2,  WasmValueType::F64          ,   WasmOpcodeOperandKind::NONE        ) \
F( F64_MIN,            0xA4,         false,    0,  2,  WasmValueType::F64          ,   WasmOpcodeOperandKind::NONE        ) \
F( F64_MAX,            0xA5,         false,    0,  2,  WasmValueType::F64          ,   WasmOpcodeOperandKind::NONE        ) \
F( F64_COPYSIGN,       0xA6,         false,    0,  2,  WasmValueType::F64          ,   WasmOpcodeOperandKind::NONE        ) \
                                                                                                                            \
F( I32_WRAP_I64,       0xA7,         false,    1,  0,  WasmValueType::I32          ,   WasmOpcodeOperandKind::NONE        ) \
F( I32_TRUNC_F32_S,    0xA8,         false,    0,  1,  WasmValueType::I32          ,   WasmOpcodeOperandKind::NONE        ) \
F( I32_TRUNC_F32_U,    0xA9,         false,    0,  1,  WasmValueType::I32          ,   WasmOpcodeOperandKind::NONE        ) \
F( I32_TRUNC_F64_S,    0xAA,         false,    0,  1,  WasmValueType::I32          ,   WasmOpcodeOperandKind::NONE        ) \
F( I32_TRUNC_F64_U,    0xAB,         false,    0,  1,  WasmValueType::I32          ,   WasmOpcodeOperandKind::NONE        ) \
                                                                                                                            \
F( I64_EXTEND_I32_S,   0xAC,         false,    1,  0,  WasmValueType::I64          ,   WasmOpcodeOperandKind::NONE        ) \
F( I64_EXTEND_I32_U,   0xAD,         false,    1,  0,  WasmValueType::I64          ,   WasmOpcodeOperandKind::NONE        ) \
F( I64_TRUNC_F32_S,    0xAE,         false,    0,  1,  WasmValueType::I64          ,   WasmOpcodeOperandKind::NONE        ) \
F( I64_TRUNC_F32_U,    0xAF,         false,    0,  1,  WasmValueType::I64          ,   WasmOpcodeOperandKind::NONE        ) \
F( I64_TRUNC_F64_S,    0xB0,         false,    0,  1,  WasmValueType::I64          ,   WasmOpcodeOperandKind::NONE        ) \
F( I64_TRUNC_F64_U,    0xB1,         false,    0,  1,  WasmValueType::I64          ,   WasmOpcodeOperandKind::NONE        ) \
                                                                                                                            \
F( F32_CONVERT_I32_S,  0xB2,         false,    1,  0,  WasmValueType::F32          ,   WasmOpcodeOperandKind::NONE        ) \
F( F32_CONVERT_I32_U,  0xB3,         false,    1,  0,  WasmValueType::F32          ,   WasmOpcodeOperandKind::NONE        ) \
F( F32_CONVERT_I64_S,  0xB4,         false,    1,  0,  WasmValueType::F32          ,   WasmOpcodeOperandKind::NONE        ) \
F( F32_CONVERT_I64_U,  0xB5,         false,    1,  0,  WasmValueType::F32          ,   WasmOpcodeOperandKind::NONE        ) \
F( F32_DEMOTE_F64,     0xB6,         false,    0,  1,  WasmValueType::F32          ,   WasmOpcodeOperandKind::NONE        ) \
                                                                                                                            \
F( F64_CONVERT_I32_S,  0xB7,         false,    1,  0,  WasmValueType::F64          ,   WasmOpcodeOperandKind::NONE        ) \
F( F64_CONVERT_I32_U,  0xB8,         false,    1,  0,  WasmValueType::F64          ,   WasmOpcodeOperandKind::NONE        ) \
F( F64_CONVERT_I64_S,  0xB9,         false,    1,  0,  WasmValueType::F64          ,   WasmOpcodeOperandKind::NONE        ) \
F( F64_CONVERT_I64_U,  0xBA,         false,    1,  0,  WasmValueType::F64          ,   WasmOpcodeOperandKind::NONE        ) \
F( F64_PROMOTE_F32,    0xBB,         false,    0,  1,  WasmValueType::F64          ,   WasmOpcodeOperandKind::NONE        ) \
                                                                                                                            \
F( I32_BITCAST_F32,    0xBC,         false,    0,  1,  WasmValueType::I32          ,   WasmOpcodeOperandKind::NONE        ) \
F( I64_BITCAST_F64,    0xBD,         false,    0,  1,  WasmValueType::I64          ,   WasmOpcodeOperandKind::NONE        ) \
F( F32_BITCAST_I32,    0xBE,         false,    1,  0,  WasmValueType::F32          ,   WasmOpcodeOperandKind::NONE        ) \
F( F64_BITCAST_I64,    0xBF,         false,    1,  0,  WasmValueType::F64          ,   WasmOpcodeOperandKind::NONE        ) \
                                                                                                                            \
F( I32_EXTEND_8S,      0xC0,         false,    1,  0,  WasmValueType::I32          ,   WasmOpcodeOperandKind::NONE        ) \
F( I32_EXTEND_16S,     0xC1,         false,    1,  0,  WasmValueType::I32          ,   WasmOpcodeOperandKind::NONE        ) \
F( I64_EXTEND_8S,      0xC2,         false,    1,  0,  WasmValueType::I64          ,   WasmOpcodeOperandKind::NONE        ) \
F( I64_EXTEND_16S,     0xC3,         false,    1,  0,  WasmValueType::I64          ,   WasmOpcodeOperandKind::NONE        ) \
F( I64_EXTEND_32S,     0xC4,         false,    1,  0,  WasmValueType::I64          ,   WasmOpcodeOperandKind::NONE        ) \
                                                                                                                            \
/* HACK: ops invented by us for helper */                                                                                   \
F( XX_SWITCH_SF,       0xD6,         false,    0,  0,  WasmValueType::X_END_OF_ENUM,   WasmOpcodeOperandKind::NONE        ) \
F( XX_I32_FILLPARAM,   0xD7,         false,    1,  0,  WasmValueType::X_END_OF_ENUM,   WasmOpcodeOperandKind::NONE        ) \
F( XX_I64_FILLPARAM,   0xD8,         false,    1,  0,  WasmValueType::X_END_OF_ENUM,   WasmOpcodeOperandKind::NONE        ) \
F( XX_F32_FILLPARAM,   0xD9,         false,    0,  1,  WasmValueType::X_END_OF_ENUM,   WasmOpcodeOperandKind::NONE        ) \
F( XX_F64_FILLPARAM,   0xDA,         false,    0,  1,  WasmValueType::X_END_OF_ENUM,   WasmOpcodeOperandKind::NONE        ) \
                                                                                                                            \
F( XX_I32_RETURN,      0xDB,         false,    1,  0,  WasmValueType::X_END_OF_ENUM,   WasmOpcodeOperandKind::NONE        ) \
F( XX_I64_RETURN,      0xDC,         false,    1,  0,  WasmValueType::X_END_OF_ENUM,   WasmOpcodeOperandKind::NONE        ) \
F( XX_F32_RETURN,      0xDD,         false,    0,  1,  WasmValueType::X_END_OF_ENUM,   WasmOpcodeOperandKind::NONE        ) \
F( XX_F64_RETURN,      0xDE,         false,    0,  1,  WasmValueType::X_END_OF_ENUM,   WasmOpcodeOperandKind::NONE        ) \
F( XX_NONE_RETURN,     0xDF,         false,    0,  0,  WasmValueType::X_END_OF_ENUM,   WasmOpcodeOperandKind::NONE        ) \
                                                                                                                            \
F( XX_I_DROP,          0xE0,         false,    1,  0,  WasmValueType::X_END_OF_ENUM,   WasmOpcodeOperandKind::NONE        ) \
F( XX_F_DROP,          0xE1,         false,    0,  1,  WasmValueType::X_END_OF_ENUM,   WasmOpcodeOperandKind::NONE        ) \
                                                                                                                            \
F( XX_I32_SELECT,      0xE2,         false,    3,  0,  WasmValueType::I32          ,   WasmOpcodeOperandKind::NONE        ) \
F( XX_I64_SELECT,      0xE3,         false,    3,  0,  WasmValueType::I64          ,   WasmOpcodeOperandKind::NONE        ) \
F( XX_F32_SELECT,      0xE4,         false,    1,  2,  WasmValueType::F32          ,   WasmOpcodeOperandKind::NONE        ) \
F( XX_F64_SELECT,      0xE5,         false,    1,  2,  WasmValueType::F64          ,   WasmOpcodeOperandKind::NONE        ) \
                                                                                                                            \
F( XX_I32_LOCAL_GET,   0xE6,         false,    0,  0,  WasmValueType::I32          ,   WasmOpcodeOperandKind::U32         ) \
F( XX_I64_LOCAL_GET,   0xE7,         false,    0,  0,  WasmValueType::I64          ,   WasmOpcodeOperandKind::U32         ) \
F( XX_F32_LOCAL_GET,   0xE8,         false,    0,  0,  WasmValueType::F32          ,   WasmOpcodeOperandKind::U32         ) \
F( XX_F64_LOCAL_GET,   0xE9,         false,    0,  0,  WasmValueType::F64          ,   WasmOpcodeOperandKind::U32         ) \
                                                                                                                            \
F( XX_I32_LOCAL_SET,   0xEA,         false,    1,  0,  WasmValueType::X_END_OF_ENUM,   WasmOpcodeOperandKind::U32         ) \
F( XX_I64_LOCAL_SET,   0xEB,         false,    1,  0,  WasmValueType::X_END_OF_ENUM,   WasmOpcodeOperandKind::U32         ) \
F( XX_F32_LOCAL_SET,   0xEC,         false,    0,  1,  WasmValueType::X_END_OF_ENUM,   WasmOpcodeOperandKind::U32         ) \
F( XX_F64_LOCAL_SET,   0xED,         false,    0,  1,  WasmValueType::X_END_OF_ENUM,   WasmOpcodeOperandKind::U32         ) \
                                                                                                                            \
F( XX_I32_LOCAL_TEE,   0xEE,         false,    1,  0,  WasmValueType::I32          ,   WasmOpcodeOperandKind::U32         ) \
F( XX_I64_LOCAL_TEE,   0xEF,         false,    1,  0,  WasmValueType::I64          ,   WasmOpcodeOperandKind::U32         ) \
F( XX_F32_LOCAL_TEE,   0xF0,         false,    0,  1,  WasmValueType::F32          ,   WasmOpcodeOperandKind::U32         ) \
F( XX_F64_LOCAL_TEE,   0xF1,         false,    0,  1,  WasmValueType::F64          ,   WasmOpcodeOperandKind::U32         ) \
                                                                                                                            \
F( XX_I32_GLOBAL_GET,  0xF2,         false,    0,  0,  WasmValueType::I32          ,   WasmOpcodeOperandKind::U32         ) \
F( XX_I64_GLOBAL_GET,  0xF3,         false,    0,  0,  WasmValueType::I64          ,   WasmOpcodeOperandKind::U32         ) \
F( XX_F32_GLOBAL_GET,  0xF4,         false,    0,  0,  WasmValueType::F32          ,   WasmOpcodeOperandKind::U32         ) \
F( XX_F64_GLOBAL_GET,  0xF5,         false,    0,  0,  WasmValueType::F64          ,   WasmOpcodeOperandKind::U32         ) \
                                                                                                                            \
F( XX_I32_GLOBAL_SET,  0xF6,         false,    1,  0,  WasmValueType::X_END_OF_ENUM,   WasmOpcodeOperandKind::U32         ) \
F( XX_I64_GLOBAL_SET,  0xF7,         false,    1,  0,  WasmValueType::X_END_OF_ENUM,   WasmOpcodeOperandKind::U32         ) \
F( XX_F32_GLOBAL_SET,  0xF8,         false,    0,  1,  WasmValueType::X_END_OF_ENUM,   WasmOpcodeOperandKind::U32         ) \
F( XX_F64_GLOBAL_SET,  0xF9,         false,    0,  1,  WasmValueType::X_END_OF_ENUM,   WasmOpcodeOperandKind::U32         )

enum class WasmOpcode : uint8_t
{
#define F(opcodeName, opcodeEncoding, ...) opcodeName = opcodeEncoding,
FOR_EACH_WASM_OPCODE
#undef F
X_END_OF_ENUM
};

struct alignas(64) WasmOpcodeInfoTable
{
    constexpr WasmOpcodeInfoTable()
    {
#define F(opcodeName, opcodeEncoding, ...) m_info[opcodeEncoding] = WasmOpcodeInfo::Create(__VA_ARGS__);
FOR_EACH_WASM_OPCODE
#undef F
    }

    WasmOpcodeInfo Get(uint8_t opcode) const
    {
        return m_info[opcode];
    }

    WasmOpcodeInfo Get(WasmOpcode opcode) const
    {
        return m_info[static_cast<uint8_t>(opcode)];
    }

    WasmOpcodeInfo m_info[256];
};

constexpr WasmOpcodeInfoTable g_wasmOpcodeInfoTable;

struct WasmCommonOpcodeFixups
{
    // int stack top, float stack top, constant
    //
    uint64_t m_data[5];
};

struct WasmCommonOpcodeStencil
{
    uint8_t m_contentLenBytes;
    uint8_t m_sym32FixupArrayLenBytes;
    uint8_t m_sym64FixupArrayLenBytes;

    const uint8_t* GetContentStart() const
    {
        return reinterpret_cast<const uint8_t*>(this + 1);
    }

    const uint8_t* GetFixupArraysStart() const
    {
        return GetContentStart() + m_contentLenBytes;
    }

    void Fixup(uint8_t*& destAddr, WasmCommonOpcodeFixups* input) const
    {
        memcpy(destAddr, GetContentStart(), m_contentLenBytes);
        const uint8_t* cur = GetFixupArraysStart();
        const uint8_t* sym32End = cur + m_sym32FixupArrayLenBytes;
        const uint8_t* sym64End = sym32End + m_sym64FixupArrayLenBytes;

        while (cur < sym32End)
        {
            uint8_t ord = *cur++;
            uint8_t offset = *cur++;
            assert(ord < 3 && offset + sizeof(uint32_t) <= m_contentLenBytes);
            uint32_t addend = static_cast<uint32_t>(input->m_data[ord]);
            UnalignedAddAndWriteback<uint32_t>(destAddr + offset, addend);
        }

        while (cur < sym64End)
        {
            uint8_t ord = *cur++;
            uint8_t offset = *cur++;
            assert(ord < 3 && offset + sizeof(uint64_t) <= m_contentLenBytes);
            uint64_t addend = static_cast<uint64_t>(input->m_data[ord]);
            UnalignedAddAndWriteback<uint64_t>(destAddr + offset, addend);
        }

        destAddr += m_contentLenBytes;
    }
};

class WasmCommonOpcodeManager
{
    struct BuildState
    {
        uint8_t* m_curAddr;
        std::unordered_map<const FastInterpBoilerplateBluePrint*, uint8_t*> m_cache;
    };

public:
    static constexpr int x_maxIntRegs = 3;
    static constexpr int x_maxFloatRegs = 3;

    static WasmCommonOpcodeManager* WARN_UNUSED Build()
    {
        constexpr uint32_t len = 32768;
        WasmCommonOpcodeManager* result;
        {
            void* addr = mmap(nullptr, len + sizeof(WasmCommonOpcodeManager), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
            if (addr == MAP_FAILED)
            {
                ReleaseAssert(false && "Out Of Memory");
            }
            assert(addr != nullptr);
            result = reinterpret_cast<WasmCommonOpcodeManager*>(addr);
        }

        memset(result, -1, sizeof(WasmCommonOpcodeManager));

        BuildState buildState;
        buildState.m_curAddr = reinterpret_cast<uint8_t*>(result + 1);
        buildState.m_cache.clear();
        uint8_t* curAddrLimit = buildState.m_curAddr + len;

        auto getRegisterLoadIntegerFn = [](FastInterpTypeId dstType, FastInterpTypeId srcType)
        {
            return [=](int numIntRegs, int /*numFloatRegs*/, bool spillOutput) -> const FastInterpBoilerplateBluePrint*
            {
                if (spillOutput && numIntRegs > 1)
                {
                    return nullptr;
                }
                if (numIntRegs == 0)
                {
                    return FastInterpBoilerplateLibrary<FIMemoryLoadOpsImpl>::SelectBoilerplateBluePrint(
                                dstType,
                                srcType,
                                static_cast<FINumOpaqueIntegralParams>(0),
                                FIOpaqueParamsHelper::GetMaxOFP(),
                                false /*isInRegister*/,
                                spillOutput);
                }
                else
                {
                    return FastInterpBoilerplateLibrary<FIMemoryLoadOpsImpl>::SelectBoilerplateBluePrint(
                                dstType,
                                srcType,
                                static_cast<FINumOpaqueIntegralParams>(numIntRegs - 1),
                                FIOpaqueParamsHelper::GetMaxOFP(),
                                true /*isInRegister*/,
                                spillOutput);
                }
            };
        };

        auto getRegisterLoadFloatFn = [](FastInterpTypeId typeId)
        {
            return [=](int numIntRegs, int numFloatRegs, bool spillOutput) -> const FastInterpBoilerplateBluePrint*
            {
                if (numFloatRegs == x_maxFloatRegs && !spillOutput)
                {
                    return nullptr;
                }
                if (spillOutput && numFloatRegs != 0)
                {
                    return nullptr;
                }
                if (numIntRegs == 0)
                {
                    return FastInterpBoilerplateLibrary<FIMemoryLoadOpsImpl>::SelectBoilerplateBluePrint(
                                typeId /*dstType*/,
                                typeId /*srcType*/,
                                static_cast<FINumOpaqueIntegralParams>(0),
                                static_cast<FINumOpaqueFloatingParams>(numFloatRegs),
                                false /*isInRegister*/,
                                spillOutput);
                }
                else
                {
                    return FastInterpBoilerplateLibrary<FIMemoryLoadOpsImpl>::SelectBoilerplateBluePrint(
                                typeId /*dstType*/,
                                typeId /*srcType*/,
                                static_cast<FINumOpaqueIntegralParams>(numIntRegs - 1),
                                static_cast<FINumOpaqueFloatingParams>(numFloatRegs),
                                true /*isInRegister*/,
                                spillOutput);
                }
            };
        };

        result->InitArray(buildState, WasmOpcode::I32_LOAD, getRegisterLoadIntegerFn(FastInterpTypeId::Get<uint32_t>(), FastInterpTypeId::Get<uint32_t>()));
        result->InitArray(buildState, WasmOpcode::I64_LOAD, getRegisterLoadIntegerFn(FastInterpTypeId::Get<uint64_t>(), FastInterpTypeId::Get<uint64_t>()));
        result->InitArray(buildState, WasmOpcode::F32_LOAD, getRegisterLoadFloatFn(FastInterpTypeId::Get<float>()));
        result->InitArray(buildState, WasmOpcode::F64_LOAD, getRegisterLoadFloatFn(FastInterpTypeId::Get<double>()));

        result->InitArray(buildState, WasmOpcode::I32_LOAD_8S, getRegisterLoadIntegerFn(FastInterpTypeId::Get<uint32_t>(), FastInterpTypeId::Get<int8_t>()));
        result->InitArray(buildState, WasmOpcode::I32_LOAD_8U, getRegisterLoadIntegerFn(FastInterpTypeId::Get<uint32_t>(), FastInterpTypeId::Get<uint8_t>()));
        result->InitArray(buildState, WasmOpcode::I32_LOAD_16S, getRegisterLoadIntegerFn(FastInterpTypeId::Get<uint32_t>(), FastInterpTypeId::Get<int16_t>()));
        result->InitArray(buildState, WasmOpcode::I32_LOAD_16U, getRegisterLoadIntegerFn(FastInterpTypeId::Get<uint32_t>(), FastInterpTypeId::Get<uint16_t>()));

        result->InitArray(buildState, WasmOpcode::I64_LOAD_8S, getRegisterLoadIntegerFn(FastInterpTypeId::Get<uint64_t>(), FastInterpTypeId::Get<int8_t>()));
        result->InitArray(buildState, WasmOpcode::I64_LOAD_8U, getRegisterLoadIntegerFn(FastInterpTypeId::Get<uint64_t>(), FastInterpTypeId::Get<uint8_t>()));
        result->InitArray(buildState, WasmOpcode::I64_LOAD_16S, getRegisterLoadIntegerFn(FastInterpTypeId::Get<uint64_t>(), FastInterpTypeId::Get<int16_t>()));
        result->InitArray(buildState, WasmOpcode::I64_LOAD_16U, getRegisterLoadIntegerFn(FastInterpTypeId::Get<uint64_t>(), FastInterpTypeId::Get<uint16_t>()));
        result->InitArray(buildState, WasmOpcode::I64_LOAD_32S, getRegisterLoadIntegerFn(FastInterpTypeId::Get<uint64_t>(), FastInterpTypeId::Get<int32_t>()));
        result->InitArray(buildState, WasmOpcode::I64_LOAD_32U, getRegisterLoadIntegerFn(FastInterpTypeId::Get<uint64_t>(), FastInterpTypeId::Get<uint32_t>()));

        auto getRegisterIntegerStoreFn = [](FastInterpTypeId dstType, FastInterpTypeId srcType)
        {
            return [=](int numIntRegs, int /*numFloatRegs*/, bool /*spillOutput*/) -> const FastInterpBoilerplateBluePrint*
            {
                if (numIntRegs == 0)
                {
                    // both memory offset and value are spilled
                    //
                    return FastInterpBoilerplateLibrary<FIMemoryStoreOpsSpilledImpl>::SelectBoilerplateBluePrint(
                                dstType,
                                srcType,
                                static_cast<FINumOpaqueIntegralParams>(0),
                                FIOpaqueParamsHelper::GetMaxOFP(),
                                false /*isInRegister*/);
                }
                else if (numIntRegs == 1)
                {
                    // memory offset is spilled, but value is not
                    //
                    return FastInterpBoilerplateLibrary<FIMemoryStoreOpsSpilledImpl>::SelectBoilerplateBluePrint(
                                dstType,
                                srcType,
                                static_cast<FINumOpaqueIntegralParams>(0),
                                FIOpaqueParamsHelper::GetMaxOFP(),
                                true /*isInRegister*/);
                }
                else
                {
                    // both memory offset and value are in register
                    //
                    return FastInterpBoilerplateLibrary<FIMemoryStoreOpsNotSpilledImpl>::SelectBoilerplateBluePrint(
                                dstType,
                                srcType,
                                static_cast<FINumOpaqueIntegralParams>(numIntRegs - 2),
                                FIOpaqueParamsHelper::GetMaxOFP(),
                                true /*isInRegister*/);
                }
            };
        };

        auto getRegisterFloatStoreFn = [](FastInterpTypeId typeId)
        {
            return [=](int numIntRegs, int numFloatRegs, bool /*spillOutput*/) -> const FastInterpBoilerplateBluePrint*
            {
                if (numIntRegs == 0)
                {
                    // memory offset is spilled
                    //
                    if (numFloatRegs == 0)
                    {
                        return FastInterpBoilerplateLibrary<FIMemoryStoreOpsSpilledImpl>::SelectBoilerplateBluePrint(
                                    typeId /*dstType*/,
                                    typeId /*srcType*/,
                                    static_cast<FINumOpaqueIntegralParams>(0),
                                    static_cast<FINumOpaqueFloatingParams>(0),
                                    false /*isInRegister*/);
                    }
                    else
                    {
                        return FastInterpBoilerplateLibrary<FIMemoryStoreOpsSpilledImpl>::SelectBoilerplateBluePrint(
                                    typeId /*dstType*/,
                                    typeId /*srcType*/,
                                    static_cast<FINumOpaqueIntegralParams>(0),
                                    static_cast<FINumOpaqueFloatingParams>(numFloatRegs - 1),
                                    true /*isInRegister*/);
                    }
                }
                else
                {
                    // memory offset is not spilled
                    //
                    if (numFloatRegs == 0)
                    {
                        return FastInterpBoilerplateLibrary<FIMemoryStoreOpsNotSpilledImpl>::SelectBoilerplateBluePrint(
                                    typeId /*dstType*/,
                                    typeId /*srcType*/,
                                    static_cast<FINumOpaqueIntegralParams>(numIntRegs - 1),
                                    static_cast<FINumOpaqueFloatingParams>(0),
                                    false /*isInRegister*/);
                    }
                    else
                    {
                        return FastInterpBoilerplateLibrary<FIMemoryStoreOpsNotSpilledImpl>::SelectBoilerplateBluePrint(
                                    typeId /*dstType*/,
                                    typeId /*srcType*/,
                                    static_cast<FINumOpaqueIntegralParams>(numIntRegs - 1),
                                    static_cast<FINumOpaqueFloatingParams>(numFloatRegs - 1),
                                    true /*isInRegister*/);
                    }
                }
            };
        };

        result->InitArray(buildState, WasmOpcode::I32_STORE, getRegisterIntegerStoreFn(FastInterpTypeId::Get<uint32_t>(), FastInterpTypeId::Get<uint32_t>()));
        result->InitArray(buildState, WasmOpcode::I64_STORE, getRegisterIntegerStoreFn(FastInterpTypeId::Get<uint64_t>(), FastInterpTypeId::Get<uint64_t>()));
        result->InitArray(buildState, WasmOpcode::F32_STORE, getRegisterFloatStoreFn(FastInterpTypeId::Get<float>()));
        result->InitArray(buildState, WasmOpcode::F64_STORE, getRegisterFloatStoreFn(FastInterpTypeId::Get<double>()));

        result->InitArray(buildState, WasmOpcode::I32_STORE_8, getRegisterIntegerStoreFn(FastInterpTypeId::Get<uint8_t>(), FastInterpTypeId::Get<uint32_t>()));
        result->InitArray(buildState, WasmOpcode::I32_STORE_16, getRegisterIntegerStoreFn(FastInterpTypeId::Get<uint16_t>(), FastInterpTypeId::Get<uint32_t>()));
        result->InitArray(buildState, WasmOpcode::I64_STORE_8, getRegisterIntegerStoreFn(FastInterpTypeId::Get<uint8_t>(), FastInterpTypeId::Get<uint64_t>()));
        result->InitArray(buildState, WasmOpcode::I64_STORE_16, getRegisterIntegerStoreFn(FastInterpTypeId::Get<uint16_t>(), FastInterpTypeId::Get<uint64_t>()));
        result->InitArray(buildState, WasmOpcode::I64_STORE_32, getRegisterIntegerStoreFn(FastInterpTypeId::Get<uint32_t>(), FastInterpTypeId::Get<uint64_t>()));

        auto registerConstI32Fn = [](int numIntRegs, int /*numFloatRegs*/, bool spillOutput) -> const FastInterpBoilerplateBluePrint*
        {
            if (spillOutput && numIntRegs > 0)
            {
                return nullptr;
            }
            if (numIntRegs == x_maxIntRegs && !spillOutput)
            {
                return nullptr;
            }
            return FastInterpBoilerplateLibrary<FIConstant32Impl>::SelectBoilerplateBluePrint(
                        FastInterpTypeId::Get<int32_t>(),
                        static_cast<FINumOpaqueIntegralParams>(numIntRegs),
                        FIOpaqueParamsHelper::GetMaxOFP(),
                        spillOutput);
        };

        auto registerConstI64Fn = [](int numIntRegs, int /*numFloatRegs*/, bool spillOutput) -> const FastInterpBoilerplateBluePrint*
        {
            if (spillOutput && numIntRegs > 0) { return nullptr; }
            if (numIntRegs == x_maxIntRegs && !spillOutput) { return nullptr; }
            return FastInterpBoilerplateLibrary<FIConstant64Impl>::SelectBoilerplateBluePrint(
                        FastInterpTypeId::Get<uint64_t>(),
                        static_cast<FINumOpaqueIntegralParams>(numIntRegs),
                        FIOpaqueParamsHelper::GetMaxOFP(),
                        spillOutput);
        };

        auto registerConstF32Fn = [](int /*numIntRegs*/, int numFloatRegs, bool spillOutput) -> const FastInterpBoilerplateBluePrint*
        {
            if (spillOutput && numFloatRegs > 0) { return nullptr; }
            if (numFloatRegs == x_maxFloatRegs && !spillOutput) { return nullptr; }
            return FastInterpBoilerplateLibrary<FIConstant32Impl>::SelectBoilerplateBluePrint(
                        FastInterpTypeId::Get<float>(),
                        FIOpaqueParamsHelper::GetMaxOIP(),
                        static_cast<FINumOpaqueFloatingParams>(numFloatRegs),
                        spillOutput);
        };

        auto registerConstF64Fn = [](int /*numIntRegs*/, int numFloatRegs, bool spillOutput) -> const FastInterpBoilerplateBluePrint*
        {
            if (spillOutput && numFloatRegs > 0) { return nullptr; }
            if (numFloatRegs == x_maxFloatRegs && !spillOutput) { return nullptr; }
            return FastInterpBoilerplateLibrary<FIConstant64Impl>::SelectBoilerplateBluePrint(
                        FastInterpTypeId::Get<double>(),
                        FIOpaqueParamsHelper::GetMaxOIP(),
                        static_cast<FINumOpaqueFloatingParams>(numFloatRegs),
                        spillOutput);
        };

        result->InitArray(buildState, WasmOpcode::I32_CONST, registerConstI32Fn);
        result->InitArray(buildState, WasmOpcode::I64_CONST, registerConstI64Fn);
        result->InitArray(buildState, WasmOpcode::F32_CONST, registerConstF32Fn);
        result->InitArray(buildState, WasmOpcode::F64_CONST, registerConstF64Fn);

        auto getRegisterIntegerTestEqzFn = [](FastInterpTypeId typeId)
        {
            return [=](int numIntRegs, int /*numFloatRegs*/, bool spillOutput) -> const FastInterpBoilerplateBluePrint*
            {
                if (spillOutput && numIntRegs > 1)
                {
                    return nullptr;
                }
                if (numIntRegs == 0)
                {
                    return FastInterpBoilerplateLibrary<FITestEqzOpsImpl>::SelectBoilerplateBluePrint(
                                typeId,
                                static_cast<FINumOpaqueIntegralParams>(0),
                                FIOpaqueParamsHelper::GetMaxOFP(),
                                false /*isInRegister*/,
                                spillOutput);
                }
                else
                {
                    return FastInterpBoilerplateLibrary<FITestEqzOpsImpl>::SelectBoilerplateBluePrint(
                                typeId,
                                static_cast<FINumOpaqueIntegralParams>(numIntRegs - 1),
                                FIOpaqueParamsHelper::GetMaxOFP(),
                                true /*isInRegister*/,
                                spillOutput);
                }
            };
        };

        auto getRegisterIntegerComparisonFn = [](FastInterpTypeId typeId, WasmRelationalOps op)
        {
            return [=](int numIntRegs, int /*numFloatRegs*/, bool spillOutput) -> const FastInterpBoilerplateBluePrint*
            {
                if (spillOutput && numIntRegs > 2)
                {
                    return nullptr;
                }
                if (numIntRegs <= 1)
                {
                    return FastInterpBoilerplateLibrary<FIRelationalOpsImpl>::SelectBoilerplateBluePrint(
                                typeId,
                                op,
                                static_cast<FINumOpaqueIntegralParams>(0),
                                FIOpaqueParamsHelper::GetMaxOFP(),
                                static_cast<NumInRegisterOperands>(numIntRegs),
                                spillOutput);
                }
                else
                {
                    return FastInterpBoilerplateLibrary<FIRelationalOpsImpl>::SelectBoilerplateBluePrint(
                                typeId,
                                op,
                                static_cast<FINumOpaqueIntegralParams>(numIntRegs - 2),
                                FIOpaqueParamsHelper::GetMaxOFP(),
                                NumInRegisterOperands::TWO,
                                spillOutput);
                }
            };
        };

        result->InitArray(buildState, WasmOpcode::I32_EQZ, getRegisterIntegerTestEqzFn(FastInterpTypeId::Get<uint32_t>()));

        result->InitArray(buildState, WasmOpcode::I32_EQ, getRegisterIntegerComparisonFn(FastInterpTypeId::Get<uint32_t>(), WasmRelationalOps::Equal));
        result->InitArray(buildState, WasmOpcode::I32_NE, getRegisterIntegerComparisonFn(FastInterpTypeId::Get<uint32_t>(), WasmRelationalOps::NotEqual));
        result->InitArray(buildState, WasmOpcode::I32_LT_S, getRegisterIntegerComparisonFn(FastInterpTypeId::Get<int32_t>(), WasmRelationalOps::LessThan));
        result->InitArray(buildState, WasmOpcode::I32_LT_U, getRegisterIntegerComparisonFn(FastInterpTypeId::Get<uint32_t>(), WasmRelationalOps::LessThan));
        result->InitArray(buildState, WasmOpcode::I32_GT_S, getRegisterIntegerComparisonFn(FastInterpTypeId::Get<int32_t>(), WasmRelationalOps::GreaterThan));
        result->InitArray(buildState, WasmOpcode::I32_GT_U, getRegisterIntegerComparisonFn(FastInterpTypeId::Get<uint32_t>(), WasmRelationalOps::GreaterThan));
        result->InitArray(buildState, WasmOpcode::I32_LE_S, getRegisterIntegerComparisonFn(FastInterpTypeId::Get<int32_t>(), WasmRelationalOps::LessEqual));
        result->InitArray(buildState, WasmOpcode::I32_LE_U, getRegisterIntegerComparisonFn(FastInterpTypeId::Get<uint32_t>(), WasmRelationalOps::LessEqual));
        result->InitArray(buildState, WasmOpcode::I32_GE_S, getRegisterIntegerComparisonFn(FastInterpTypeId::Get<int32_t>(), WasmRelationalOps::GreaterEqual));
        result->InitArray(buildState, WasmOpcode::I32_GE_U, getRegisterIntegerComparisonFn(FastInterpTypeId::Get<uint32_t>(), WasmRelationalOps::GreaterEqual));

        result->InitArray(buildState, WasmOpcode::I64_EQZ, getRegisterIntegerTestEqzFn(FastInterpTypeId::Get<uint64_t>()));

        result->InitArray(buildState, WasmOpcode::I64_EQ, getRegisterIntegerComparisonFn(FastInterpTypeId::Get<uint64_t>(), WasmRelationalOps::Equal));
        result->InitArray(buildState, WasmOpcode::I64_NE, getRegisterIntegerComparisonFn(FastInterpTypeId::Get<uint64_t>(), WasmRelationalOps::NotEqual));
        result->InitArray(buildState, WasmOpcode::I64_LT_S, getRegisterIntegerComparisonFn(FastInterpTypeId::Get<int64_t>(), WasmRelationalOps::LessThan));
        result->InitArray(buildState, WasmOpcode::I64_LT_U, getRegisterIntegerComparisonFn(FastInterpTypeId::Get<uint64_t>(), WasmRelationalOps::LessThan));
        result->InitArray(buildState, WasmOpcode::I64_GT_S, getRegisterIntegerComparisonFn(FastInterpTypeId::Get<int64_t>(), WasmRelationalOps::GreaterThan));
        result->InitArray(buildState, WasmOpcode::I64_GT_U, getRegisterIntegerComparisonFn(FastInterpTypeId::Get<uint64_t>(), WasmRelationalOps::GreaterThan));
        result->InitArray(buildState, WasmOpcode::I64_LE_S, getRegisterIntegerComparisonFn(FastInterpTypeId::Get<int64_t>(), WasmRelationalOps::LessEqual));
        result->InitArray(buildState, WasmOpcode::I64_LE_U, getRegisterIntegerComparisonFn(FastInterpTypeId::Get<uint64_t>(), WasmRelationalOps::LessEqual));
        result->InitArray(buildState, WasmOpcode::I64_GE_S, getRegisterIntegerComparisonFn(FastInterpTypeId::Get<int64_t>(), WasmRelationalOps::GreaterEqual));
        result->InitArray(buildState, WasmOpcode::I64_GE_U, getRegisterIntegerComparisonFn(FastInterpTypeId::Get<uint64_t>(), WasmRelationalOps::GreaterEqual));

        auto getRegisterFloatComparisonFn = [](FastInterpTypeId typeId, WasmRelationalOps op)
        {
            return [=](int numIntRegs, int numFloatRegs, bool spillOutput) -> const FastInterpBoilerplateBluePrint*
            {
                if (numIntRegs == x_maxIntRegs && !spillOutput)
                {
                    return nullptr;
                }
                if (spillOutput && numIntRegs != 0)
                {
                    return nullptr;
                }
                if (numFloatRegs <= 1)
                {
                    return FastInterpBoilerplateLibrary<FIRelationalOpsImpl>::SelectBoilerplateBluePrint(
                                typeId,
                                op,
                                static_cast<FINumOpaqueIntegralParams>(numIntRegs),
                                static_cast<FINumOpaqueFloatingParams>(0),
                                static_cast<NumInRegisterOperands>(numFloatRegs),
                                spillOutput);
                }
                else
                {
                    return FastInterpBoilerplateLibrary<FIRelationalOpsImpl>::SelectBoilerplateBluePrint(
                                typeId,
                                op,
                                static_cast<FINumOpaqueIntegralParams>(numIntRegs),
                                static_cast<FINumOpaqueFloatingParams>(numFloatRegs - 2),
                                NumInRegisterOperands::TWO,
                                spillOutput);
                }
            };
        };

        result->InitArray(buildState, WasmOpcode::F32_EQ, getRegisterFloatComparisonFn(FastInterpTypeId::Get<float>(), WasmRelationalOps::Equal));
        result->InitArray(buildState, WasmOpcode::F32_NE, getRegisterFloatComparisonFn(FastInterpTypeId::Get<float>(), WasmRelationalOps::NotEqual));
        result->InitArray(buildState, WasmOpcode::F32_LT, getRegisterFloatComparisonFn(FastInterpTypeId::Get<float>(), WasmRelationalOps::LessThan));
        result->InitArray(buildState, WasmOpcode::F32_GT, getRegisterFloatComparisonFn(FastInterpTypeId::Get<float>(), WasmRelationalOps::GreaterThan));
        result->InitArray(buildState, WasmOpcode::F32_LE, getRegisterFloatComparisonFn(FastInterpTypeId::Get<float>(), WasmRelationalOps::LessEqual));
        result->InitArray(buildState, WasmOpcode::F32_GE, getRegisterFloatComparisonFn(FastInterpTypeId::Get<float>(), WasmRelationalOps::GreaterEqual));

        result->InitArray(buildState, WasmOpcode::F64_EQ, getRegisterFloatComparisonFn(FastInterpTypeId::Get<double>(), WasmRelationalOps::Equal));
        result->InitArray(buildState, WasmOpcode::F64_NE, getRegisterFloatComparisonFn(FastInterpTypeId::Get<double>(), WasmRelationalOps::NotEqual));
        result->InitArray(buildState, WasmOpcode::F64_LT, getRegisterFloatComparisonFn(FastInterpTypeId::Get<double>(), WasmRelationalOps::LessThan));
        result->InitArray(buildState, WasmOpcode::F64_GT, getRegisterFloatComparisonFn(FastInterpTypeId::Get<double>(), WasmRelationalOps::GreaterThan));
        result->InitArray(buildState, WasmOpcode::F64_LE, getRegisterFloatComparisonFn(FastInterpTypeId::Get<double>(), WasmRelationalOps::LessEqual));
        result->InitArray(buildState, WasmOpcode::F64_GE, getRegisterFloatComparisonFn(FastInterpTypeId::Get<double>(), WasmRelationalOps::GreaterEqual));

        auto getRegisterIntegerUnaryOpsFn = [](FastInterpTypeId typeId, WasmIntUnaryOps op)
        {
            return [=](int numIntRegs, int /*numFloatRegs*/, bool spillOutput) -> const FastInterpBoilerplateBluePrint*
            {
                if (spillOutput && numIntRegs > 1)
                {
                    return nullptr;
                }
                if (numIntRegs == 0)
                {
                    return FastInterpBoilerplateLibrary<FIIntUnaryOpsImpl>::SelectBoilerplateBluePrint(
                                typeId,
                                op,
                                static_cast<FINumOpaqueIntegralParams>(0),
                                FIOpaqueParamsHelper::GetMaxOFP(),
                                false /*isInRegister*/,
                                spillOutput);
                }
                else
                {
                    return FastInterpBoilerplateLibrary<FIIntUnaryOpsImpl>::SelectBoilerplateBluePrint(
                                typeId,
                                op,
                                static_cast<FINumOpaqueIntegralParams>(numIntRegs - 1),
                                FIOpaqueParamsHelper::GetMaxOFP(),
                                true /*isInRegister*/,
                                spillOutput);
                }
            };
        };

        auto getRegisterIntegerBinaryOpsFn = [](FastInterpTypeId typeId, WasmIntBinaryOps op)
        {
            return [=](int numIntRegs, int /*numFloatRegs*/, bool spillOutput) -> const FastInterpBoilerplateBluePrint*
            {
                if (spillOutput && numIntRegs > 2)
                {
                    return nullptr;
                }
                if (numIntRegs <= 1)
                {
                    return FastInterpBoilerplateLibrary<FIIntBinaryOpsImpl>::SelectBoilerplateBluePrint(
                                typeId,
                                op,
                                static_cast<FINumOpaqueIntegralParams>(0),
                                FIOpaqueParamsHelper::GetMaxOFP(),
                                static_cast<NumInRegisterOperands>(numIntRegs),
                                spillOutput);
                }
                else
                {
                    return FastInterpBoilerplateLibrary<FIIntBinaryOpsImpl>::SelectBoilerplateBluePrint(
                                typeId,
                                op,
                                static_cast<FINumOpaqueIntegralParams>(numIntRegs - 2),
                                FIOpaqueParamsHelper::GetMaxOFP(),
                                NumInRegisterOperands::TWO,
                                spillOutput);
                }
            };
        };

        result->InitArray(buildState, WasmOpcode::I32_CLZ, getRegisterIntegerUnaryOpsFn(FastInterpTypeId::Get<uint32_t>(), WasmIntUnaryOps::Clz));
        result->InitArray(buildState, WasmOpcode::I32_CTZ, getRegisterIntegerUnaryOpsFn(FastInterpTypeId::Get<uint32_t>(), WasmIntUnaryOps::Ctz));
        result->InitArray(buildState, WasmOpcode::I32_POPCNT, getRegisterIntegerUnaryOpsFn(FastInterpTypeId::Get<uint32_t>(), WasmIntUnaryOps::Popcnt));

        result->InitArray(buildState, WasmOpcode::I32_ADD, getRegisterIntegerBinaryOpsFn(FastInterpTypeId::Get<uint32_t>(), WasmIntBinaryOps::Add));
        result->InitArray(buildState, WasmOpcode::I32_SUB, getRegisterIntegerBinaryOpsFn(FastInterpTypeId::Get<uint32_t>(), WasmIntBinaryOps::Sub));
        result->InitArray(buildState, WasmOpcode::I32_MUL, getRegisterIntegerBinaryOpsFn(FastInterpTypeId::Get<uint32_t>(), WasmIntBinaryOps::Mul));
        result->InitArray(buildState, WasmOpcode::I32_DIV_S, getRegisterIntegerBinaryOpsFn(FastInterpTypeId::Get<int32_t>(), WasmIntBinaryOps::Div));
        result->InitArray(buildState, WasmOpcode::I32_DIV_U, getRegisterIntegerBinaryOpsFn(FastInterpTypeId::Get<uint32_t>(), WasmIntBinaryOps::Div));
        result->InitArray(buildState, WasmOpcode::I32_REM_S, getRegisterIntegerBinaryOpsFn(FastInterpTypeId::Get<int32_t>(), WasmIntBinaryOps::Rem));
        result->InitArray(buildState, WasmOpcode::I32_REM_U, getRegisterIntegerBinaryOpsFn(FastInterpTypeId::Get<uint32_t>(), WasmIntBinaryOps::Rem));
        result->InitArray(buildState, WasmOpcode::I32_AND, getRegisterIntegerBinaryOpsFn(FastInterpTypeId::Get<uint32_t>(), WasmIntBinaryOps::And));
        result->InitArray(buildState, WasmOpcode::I32_OR, getRegisterIntegerBinaryOpsFn(FastInterpTypeId::Get<uint32_t>(), WasmIntBinaryOps::Or));
        result->InitArray(buildState, WasmOpcode::I32_XOR, getRegisterIntegerBinaryOpsFn(FastInterpTypeId::Get<uint32_t>(), WasmIntBinaryOps::Xor));
        result->InitArray(buildState, WasmOpcode::I32_SHL, getRegisterIntegerBinaryOpsFn(FastInterpTypeId::Get<uint32_t>(), WasmIntBinaryOps::Shl));
        result->InitArray(buildState, WasmOpcode::I32_SHR_S, getRegisterIntegerBinaryOpsFn(FastInterpTypeId::Get<int32_t>(), WasmIntBinaryOps::Shr));
        result->InitArray(buildState, WasmOpcode::I32_SHR_U, getRegisterIntegerBinaryOpsFn(FastInterpTypeId::Get<uint32_t>(), WasmIntBinaryOps::Shr));
        result->InitArray(buildState, WasmOpcode::I32_ROTL, getRegisterIntegerBinaryOpsFn(FastInterpTypeId::Get<uint32_t>(), WasmIntBinaryOps::Rotl));
        result->InitArray(buildState, WasmOpcode::I32_ROTR, getRegisterIntegerBinaryOpsFn(FastInterpTypeId::Get<uint32_t>(), WasmIntBinaryOps::Rotr));

        result->InitArray(buildState, WasmOpcode::I64_CLZ, getRegisterIntegerUnaryOpsFn(FastInterpTypeId::Get<uint64_t>(), WasmIntUnaryOps::Clz));
        result->InitArray(buildState, WasmOpcode::I64_CTZ, getRegisterIntegerUnaryOpsFn(FastInterpTypeId::Get<uint64_t>(), WasmIntUnaryOps::Ctz));
        result->InitArray(buildState, WasmOpcode::I64_POPCNT, getRegisterIntegerUnaryOpsFn(FastInterpTypeId::Get<uint64_t>(), WasmIntUnaryOps::Popcnt));

        result->InitArray(buildState, WasmOpcode::I64_ADD, getRegisterIntegerBinaryOpsFn(FastInterpTypeId::Get<uint64_t>(), WasmIntBinaryOps::Add));
        result->InitArray(buildState, WasmOpcode::I64_SUB, getRegisterIntegerBinaryOpsFn(FastInterpTypeId::Get<uint64_t>(), WasmIntBinaryOps::Sub));
        result->InitArray(buildState, WasmOpcode::I64_MUL, getRegisterIntegerBinaryOpsFn(FastInterpTypeId::Get<uint64_t>(), WasmIntBinaryOps::Mul));
        result->InitArray(buildState, WasmOpcode::I64_DIV_S, getRegisterIntegerBinaryOpsFn(FastInterpTypeId::Get<int64_t>(), WasmIntBinaryOps::Div));
        result->InitArray(buildState, WasmOpcode::I64_DIV_U, getRegisterIntegerBinaryOpsFn(FastInterpTypeId::Get<uint64_t>(), WasmIntBinaryOps::Div));
        result->InitArray(buildState, WasmOpcode::I64_REM_S, getRegisterIntegerBinaryOpsFn(FastInterpTypeId::Get<int64_t>(), WasmIntBinaryOps::Rem));
        result->InitArray(buildState, WasmOpcode::I64_REM_U, getRegisterIntegerBinaryOpsFn(FastInterpTypeId::Get<uint64_t>(), WasmIntBinaryOps::Rem));
        result->InitArray(buildState, WasmOpcode::I64_AND, getRegisterIntegerBinaryOpsFn(FastInterpTypeId::Get<uint64_t>(), WasmIntBinaryOps::And));
        result->InitArray(buildState, WasmOpcode::I64_OR, getRegisterIntegerBinaryOpsFn(FastInterpTypeId::Get<uint64_t>(), WasmIntBinaryOps::Or));
        result->InitArray(buildState, WasmOpcode::I64_XOR, getRegisterIntegerBinaryOpsFn(FastInterpTypeId::Get<uint64_t>(), WasmIntBinaryOps::Xor));
        result->InitArray(buildState, WasmOpcode::I64_SHL, getRegisterIntegerBinaryOpsFn(FastInterpTypeId::Get<uint64_t>(), WasmIntBinaryOps::Shl));
        result->InitArray(buildState, WasmOpcode::I64_SHR_S, getRegisterIntegerBinaryOpsFn(FastInterpTypeId::Get<int64_t>(), WasmIntBinaryOps::Shr));
        result->InitArray(buildState, WasmOpcode::I64_SHR_U, getRegisterIntegerBinaryOpsFn(FastInterpTypeId::Get<uint64_t>(), WasmIntBinaryOps::Shr));
        result->InitArray(buildState, WasmOpcode::I64_ROTL, getRegisterIntegerBinaryOpsFn(FastInterpTypeId::Get<uint64_t>(), WasmIntBinaryOps::Rotl));
        result->InitArray(buildState, WasmOpcode::I64_ROTR, getRegisterIntegerBinaryOpsFn(FastInterpTypeId::Get<uint64_t>(), WasmIntBinaryOps::Rotr));

        auto getRegisterFloatUnaryOpsFn = [](FastInterpTypeId typeId, WasmFloatUnaryOps op)
        {
            return [=](int /*numIntRegs*/, int numFloatRegs, bool spillOutput) -> const FastInterpBoilerplateBluePrint*
            {
                if (spillOutput && numFloatRegs > 1)
                {
                    return nullptr;
                }
                if (numFloatRegs == 0)
                {
                    return FastInterpBoilerplateLibrary<FIFloatUnaryOpsImpl>::SelectBoilerplateBluePrint(
                                typeId,
                                op,
                                FIOpaqueParamsHelper::GetMaxOIP(),
                                static_cast<FINumOpaqueFloatingParams>(0),
                                false /*isInRegister*/,
                                spillOutput);
                }
                else
                {
                    return FastInterpBoilerplateLibrary<FIFloatUnaryOpsImpl>::SelectBoilerplateBluePrint(
                                typeId,
                                op,
                                FIOpaqueParamsHelper::GetMaxOIP(),
                                static_cast<FINumOpaqueFloatingParams>(numFloatRegs - 1),
                                true /*isInRegister*/,
                                spillOutput);
                }
            };
        };

        auto getRegisterFloatBinaryOpsFn = [](FastInterpTypeId typeId, WasmFloatBinaryOps op)
        {
            return [=](int /*numIntRegs*/, int numFloatRegs, bool spillOutput) -> const FastInterpBoilerplateBluePrint*
            {
                if (spillOutput && numFloatRegs > 2)
                {
                    return nullptr;
                }
                if (numFloatRegs <= 1)
                {
                    return FastInterpBoilerplateLibrary<FIFloatBinaryOpsImpl>::SelectBoilerplateBluePrint(
                                typeId,
                                op,
                                FIOpaqueParamsHelper::GetMaxOIP(),
                                static_cast<FINumOpaqueFloatingParams>(0),
                                static_cast<NumInRegisterOperands>(numFloatRegs),
                                spillOutput);
                }
                else
                {
                    return FastInterpBoilerplateLibrary<FIFloatBinaryOpsImpl>::SelectBoilerplateBluePrint(
                                typeId,
                                op,
                                FIOpaqueParamsHelper::GetMaxOIP(),
                                static_cast<FINumOpaqueFloatingParams>(numFloatRegs - 2),
                                NumInRegisterOperands::TWO,
                                spillOutput);
                }
            };
        };

        result->InitArray(buildState, WasmOpcode::F32_ABS, getRegisterFloatUnaryOpsFn(FastInterpTypeId::Get<float>(), WasmFloatUnaryOps::Abs));
        result->InitArray(buildState, WasmOpcode::F32_NEG, getRegisterFloatUnaryOpsFn(FastInterpTypeId::Get<float>(), WasmFloatUnaryOps::Neg));
        result->InitArray(buildState, WasmOpcode::F32_CEIL, getRegisterFloatUnaryOpsFn(FastInterpTypeId::Get<float>(), WasmFloatUnaryOps::Ceil));
        result->InitArray(buildState, WasmOpcode::F32_FLOOR, getRegisterFloatUnaryOpsFn(FastInterpTypeId::Get<float>(), WasmFloatUnaryOps::Floor));
        result->InitArray(buildState, WasmOpcode::F32_TRUNC, getRegisterFloatUnaryOpsFn(FastInterpTypeId::Get<float>(), WasmFloatUnaryOps::Trunc));
        result->InitArray(buildState, WasmOpcode::F32_NEAREST, getRegisterFloatUnaryOpsFn(FastInterpTypeId::Get<float>(), WasmFloatUnaryOps::Nearest));
        result->InitArray(buildState, WasmOpcode::F32_SQRT, getRegisterFloatUnaryOpsFn(FastInterpTypeId::Get<float>(), WasmFloatUnaryOps::Sqrt));

        result->InitArray(buildState, WasmOpcode::F32_ADD, getRegisterFloatBinaryOpsFn(FastInterpTypeId::Get<float>(), WasmFloatBinaryOps::Add));
        result->InitArray(buildState, WasmOpcode::F32_SUB, getRegisterFloatBinaryOpsFn(FastInterpTypeId::Get<float>(), WasmFloatBinaryOps::Sub));
        result->InitArray(buildState, WasmOpcode::F32_MUL, getRegisterFloatBinaryOpsFn(FastInterpTypeId::Get<float>(), WasmFloatBinaryOps::Mul));
        result->InitArray(buildState, WasmOpcode::F32_DIV, getRegisterFloatBinaryOpsFn(FastInterpTypeId::Get<float>(), WasmFloatBinaryOps::Div));
        result->InitArray(buildState, WasmOpcode::F32_MIN, getRegisterFloatBinaryOpsFn(FastInterpTypeId::Get<float>(), WasmFloatBinaryOps::Min));
        result->InitArray(buildState, WasmOpcode::F32_MAX, getRegisterFloatBinaryOpsFn(FastInterpTypeId::Get<float>(), WasmFloatBinaryOps::Max));
        result->InitArray(buildState, WasmOpcode::F32_COPYSIGN, getRegisterFloatBinaryOpsFn(FastInterpTypeId::Get<float>(), WasmFloatBinaryOps::CopySign));

        result->InitArray(buildState, WasmOpcode::F64_ABS, getRegisterFloatUnaryOpsFn(FastInterpTypeId::Get<double>(), WasmFloatUnaryOps::Abs));
        result->InitArray(buildState, WasmOpcode::F64_NEG, getRegisterFloatUnaryOpsFn(FastInterpTypeId::Get<double>(), WasmFloatUnaryOps::Neg));
        result->InitArray(buildState, WasmOpcode::F64_CEIL, getRegisterFloatUnaryOpsFn(FastInterpTypeId::Get<double>(), WasmFloatUnaryOps::Ceil));
        result->InitArray(buildState, WasmOpcode::F64_FLOOR, getRegisterFloatUnaryOpsFn(FastInterpTypeId::Get<double>(), WasmFloatUnaryOps::Floor));
        result->InitArray(buildState, WasmOpcode::F64_TRUNC, getRegisterFloatUnaryOpsFn(FastInterpTypeId::Get<double>(), WasmFloatUnaryOps::Trunc));
        result->InitArray(buildState, WasmOpcode::F64_NEAREST, getRegisterFloatUnaryOpsFn(FastInterpTypeId::Get<double>(), WasmFloatUnaryOps::Nearest));
        result->InitArray(buildState, WasmOpcode::F64_SQRT, getRegisterFloatUnaryOpsFn(FastInterpTypeId::Get<double>(), WasmFloatUnaryOps::Sqrt));

        result->InitArray(buildState, WasmOpcode::F64_ADD, getRegisterFloatBinaryOpsFn(FastInterpTypeId::Get<double>(), WasmFloatBinaryOps::Add));
        result->InitArray(buildState, WasmOpcode::F64_SUB, getRegisterFloatBinaryOpsFn(FastInterpTypeId::Get<double>(), WasmFloatBinaryOps::Sub));
        result->InitArray(buildState, WasmOpcode::F64_MUL, getRegisterFloatBinaryOpsFn(FastInterpTypeId::Get<double>(), WasmFloatBinaryOps::Mul));
        result->InitArray(buildState, WasmOpcode::F64_DIV, getRegisterFloatBinaryOpsFn(FastInterpTypeId::Get<double>(), WasmFloatBinaryOps::Div));
        result->InitArray(buildState, WasmOpcode::F64_MIN, getRegisterFloatBinaryOpsFn(FastInterpTypeId::Get<double>(), WasmFloatBinaryOps::Min));
        result->InitArray(buildState, WasmOpcode::F64_MAX, getRegisterFloatBinaryOpsFn(FastInterpTypeId::Get<double>(), WasmFloatBinaryOps::Max));
        result->InitArray(buildState, WasmOpcode::F64_COPYSIGN, getRegisterFloatBinaryOpsFn(FastInterpTypeId::Get<double>(), WasmFloatBinaryOps::CopySign));

        auto getRegisterConversionBetweenIntsFn = [](FastInterpTypeId srcType, FastInterpTypeId dstType)
        {
            ReleaseAssert(srcType.GetTypeId().IsPrimitiveIntType() && dstType.GetTypeId().IsPrimitiveIntType());
            return [=](int numIntRegs, int /*numFloatRegs*/, bool spillOutput) -> const FastInterpBoilerplateBluePrint*
            {
                if (spillOutput && numIntRegs > 1)
                {
                    return nullptr;
                }
                if (numIntRegs == 0)
                {
                    return FastInterpBoilerplateLibrary<FIConversionOpsImpl>::SelectBoilerplateBluePrint(
                                srcType,
                                dstType,
                                static_cast<FINumOpaqueIntegralParams>(0),
                                FIOpaqueParamsHelper::GetMaxOFP(),
                                false /*isInRegister*/,
                                spillOutput);
                }
                else
                {
                    return FastInterpBoilerplateLibrary<FIConversionOpsImpl>::SelectBoilerplateBluePrint(
                                srcType,
                                dstType,
                                static_cast<FINumOpaqueIntegralParams>(numIntRegs - 1),
                                FIOpaqueParamsHelper::GetMaxOFP(),
                                true /*isInRegister*/,
                                spillOutput);
                }
            };
        };

        auto getRegisterConversionIntToFloatFn = [](FastInterpTypeId srcType, FastInterpTypeId dstType)
        {
            ReleaseAssert(srcType.GetTypeId().IsPrimitiveIntType() && dstType.GetTypeId().IsFloatingPoint());
            return [=](int numIntRegs, int numFloatRegs, bool spillOutput) -> const FastInterpBoilerplateBluePrint*
            {
                if (spillOutput && numFloatRegs != 0)
                {
                    return nullptr;
                }
                if (numFloatRegs == x_maxFloatRegs && !spillOutput)
                {
                    return nullptr;
                }
                if (numIntRegs == 0)
                {
                    return FastInterpBoilerplateLibrary<FIConversionOpsImpl>::SelectBoilerplateBluePrint(
                                srcType,
                                dstType,
                                static_cast<FINumOpaqueIntegralParams>(0),
                                static_cast<FINumOpaqueFloatingParams>(numFloatRegs),
                                false /*isInRegister*/,
                                spillOutput);
                }
                else
                {
                    return FastInterpBoilerplateLibrary<FIConversionOpsImpl>::SelectBoilerplateBluePrint(
                                srcType,
                                dstType,
                                static_cast<FINumOpaqueIntegralParams>(numIntRegs - 1),
                                static_cast<FINumOpaqueFloatingParams>(numFloatRegs),
                                true /*isInRegister*/,
                                spillOutput);
                }
            };
        };

        auto getRegisterConversionFloatToIntFn = [](FastInterpTypeId srcType, FastInterpTypeId dstType)
        {
            ReleaseAssert(srcType.GetTypeId().IsFloatingPoint() && dstType.GetTypeId().IsPrimitiveIntType());
            return [=](int numIntRegs, int numFloatRegs, bool spillOutput) -> const FastInterpBoilerplateBluePrint*
            {
                if (spillOutput && numIntRegs != 0)
                {
                    return nullptr;
                }
                if (numIntRegs == x_maxIntRegs && !spillOutput)
                {
                    return nullptr;
                }
                if (numFloatRegs == 0)
                {
                    return FastInterpBoilerplateLibrary<FIConversionOpsImpl>::SelectBoilerplateBluePrint(
                                srcType,
                                dstType,
                                static_cast<FINumOpaqueIntegralParams>(numIntRegs),
                                static_cast<FINumOpaqueFloatingParams>(0),
                                false /*isInRegister*/,
                                spillOutput);
                }
                else
                {
                    return FastInterpBoilerplateLibrary<FIConversionOpsImpl>::SelectBoilerplateBluePrint(
                                srcType,
                                dstType,
                                static_cast<FINumOpaqueIntegralParams>(numIntRegs),
                                static_cast<FINumOpaqueFloatingParams>(numFloatRegs - 1),
                                true /*isInRegister*/,
                                spillOutput);
                }
            };
        };

        auto getRegisterConversionBetweenFloatsFn = [](FastInterpTypeId srcType, FastInterpTypeId dstType)
        {
            ReleaseAssert(srcType.GetTypeId().IsFloatingPoint() && dstType.GetTypeId().IsFloatingPoint());
            return [=](int /*numIntRegs*/, int numFloatRegs, bool spillOutput) -> const FastInterpBoilerplateBluePrint*
            {
                if (spillOutput && numFloatRegs > 1)
                {
                    return nullptr;
                }
                if (numFloatRegs == 0)
                {
                    return FastInterpBoilerplateLibrary<FIConversionOpsImpl>::SelectBoilerplateBluePrint(
                                srcType,
                                dstType,
                                FIOpaqueParamsHelper::GetMaxOIP(),
                                static_cast<FINumOpaqueFloatingParams>(0),
                                false /*isInRegister*/,
                                spillOutput);
                }
                else
                {
                    return FastInterpBoilerplateLibrary<FIConversionOpsImpl>::SelectBoilerplateBluePrint(
                                srcType,
                                dstType,
                                FIOpaqueParamsHelper::GetMaxOIP(),
                                static_cast<FINumOpaqueFloatingParams>(numFloatRegs - 1),
                                true /*isInRegister*/,
                                spillOutput);
                }
            };
        };

        result->InitArray(buildState, WasmOpcode::I32_WRAP_I64, getRegisterConversionBetweenIntsFn(FastInterpTypeId::Get<uint64_t>(), FastInterpTypeId::Get<uint32_t>()));
        result->InitArray(buildState, WasmOpcode::I32_TRUNC_F32_S, getRegisterConversionFloatToIntFn(FastInterpTypeId::Get<float>(), FastInterpTypeId::Get<int32_t>()));
        result->InitArray(buildState, WasmOpcode::I32_TRUNC_F32_U, getRegisterConversionFloatToIntFn(FastInterpTypeId::Get<float>(), FastInterpTypeId::Get<uint32_t>()));
        result->InitArray(buildState, WasmOpcode::I32_TRUNC_F64_S, getRegisterConversionFloatToIntFn(FastInterpTypeId::Get<double>(), FastInterpTypeId::Get<int32_t>()));
        result->InitArray(buildState, WasmOpcode::I32_TRUNC_F64_U, getRegisterConversionFloatToIntFn(FastInterpTypeId::Get<double>(), FastInterpTypeId::Get<uint32_t>()));

        result->InitArray(buildState, WasmOpcode::I64_EXTEND_I32_S, getRegisterConversionBetweenIntsFn(FastInterpTypeId::Get<int32_t>(), FastInterpTypeId::Get<int64_t>()));
        result->InitArray(buildState, WasmOpcode::I64_EXTEND_I32_U, getRegisterConversionBetweenIntsFn(FastInterpTypeId::Get<uint32_t>(), FastInterpTypeId::Get<uint64_t>()));
        result->InitArray(buildState, WasmOpcode::I64_TRUNC_F32_S, getRegisterConversionFloatToIntFn(FastInterpTypeId::Get<float>(), FastInterpTypeId::Get<int64_t>()));
        // TODO: FIXME now we trunc to int64_t instead of uint64_t, figure out overflow later
        result->InitArray(buildState, WasmOpcode::I64_TRUNC_F32_U, getRegisterConversionFloatToIntFn(FastInterpTypeId::Get<float>(), FastInterpTypeId::Get<int64_t>()));
        result->InitArray(buildState, WasmOpcode::I64_TRUNC_F64_S, getRegisterConversionFloatToIntFn(FastInterpTypeId::Get<double>(), FastInterpTypeId::Get<int64_t>()));
        // TODO: FIXME now we trunc to int64_t instead of uint64_t, figure out overflow later
        result->InitArray(buildState, WasmOpcode::I64_TRUNC_F64_U, getRegisterConversionFloatToIntFn(FastInterpTypeId::Get<double>(), FastInterpTypeId::Get<int64_t>()));

        result->InitArray(buildState, WasmOpcode::F32_CONVERT_I32_S, getRegisterConversionIntToFloatFn(FastInterpTypeId::Get<int32_t>(), FastInterpTypeId::Get<float>()));
        result->InitArray(buildState, WasmOpcode::F32_CONVERT_I32_U, getRegisterConversionIntToFloatFn(FastInterpTypeId::Get<uint32_t>(), FastInterpTypeId::Get<float>()));
        result->InitArray(buildState, WasmOpcode::F32_CONVERT_I64_S, getRegisterConversionIntToFloatFn(FastInterpTypeId::Get<int64_t>(), FastInterpTypeId::Get<float>()));
        result->InitArray(buildState, WasmOpcode::F32_CONVERT_I64_U, getRegisterConversionIntToFloatFn(FastInterpTypeId::Get<uint64_t>(), FastInterpTypeId::Get<float>()));
        result->InitArray(buildState, WasmOpcode::F32_DEMOTE_F64, getRegisterConversionBetweenFloatsFn(FastInterpTypeId::Get<double>(), FastInterpTypeId::Get<float>()));

        result->InitArray(buildState, WasmOpcode::F64_CONVERT_I32_S, getRegisterConversionIntToFloatFn(FastInterpTypeId::Get<int32_t>(), FastInterpTypeId::Get<double>()));
        result->InitArray(buildState, WasmOpcode::F64_CONVERT_I32_U, getRegisterConversionIntToFloatFn(FastInterpTypeId::Get<uint32_t>(), FastInterpTypeId::Get<double>()));
        result->InitArray(buildState, WasmOpcode::F64_CONVERT_I64_S, getRegisterConversionIntToFloatFn(FastInterpTypeId::Get<int64_t>(), FastInterpTypeId::Get<double>()));
        // TODO: FIXME now we trunc from int64_t instead of uint64_t, figure out overflow later
        result->InitArray(buildState, WasmOpcode::F64_CONVERT_I64_U, getRegisterConversionIntToFloatFn(FastInterpTypeId::Get<int64_t>(), FastInterpTypeId::Get<double>()));
        result->InitArray(buildState, WasmOpcode::F64_PROMOTE_F32, getRegisterConversionBetweenFloatsFn(FastInterpTypeId::Get<float>(), FastInterpTypeId::Get<double>()));

        auto getRegisterFloatToIntBitcastFn = [](FastInterpTypeId srcType, FastInterpTypeId dstType)
        {
            return [=](int numIntRegs, int numFloatRegs, bool spillOutput) -> const FastInterpBoilerplateBluePrint*
            {
                if (spillOutput && numIntRegs > 0)
                {
                    return nullptr;
                }
                if (numIntRegs == x_maxIntRegs && !spillOutput)
                {
                    return nullptr;
                }
                if (numFloatRegs == 0)
                {
                    return FastInterpBoilerplateLibrary<FIBitcastOpsImpl>::SelectBoilerplateBluePrint(
                                srcType,
                                dstType,
                                static_cast<FINumOpaqueIntegralParams>(numIntRegs),
                                static_cast<FINumOpaqueFloatingParams>(0),
                                false /*isInRegister*/,
                                spillOutput);
                }
                else
                {
                    return FastInterpBoilerplateLibrary<FIBitcastOpsImpl>::SelectBoilerplateBluePrint(
                                srcType,
                                dstType,
                                static_cast<FINumOpaqueIntegralParams>(numIntRegs),
                                static_cast<FINumOpaqueFloatingParams>(numFloatRegs - 1),
                                true /*isInRegister*/,
                                spillOutput);
                }
            };
        };

        auto getRegisterIntToFloatBitcastFn = [](FastInterpTypeId srcType, FastInterpTypeId dstType)
        {
            return [=](int numIntRegs, int numFloatRegs, bool spillOutput) -> const FastInterpBoilerplateBluePrint*
            {
                if (spillOutput && numFloatRegs > 0)
                {
                    return nullptr;
                }
                if (numFloatRegs == x_maxFloatRegs && !spillOutput)
                {
                    return nullptr;
                }
                if (numIntRegs == 0)
                {
                    return FastInterpBoilerplateLibrary<FIBitcastOpsImpl>::SelectBoilerplateBluePrint(
                                srcType,
                                dstType,
                                static_cast<FINumOpaqueIntegralParams>(0),
                                static_cast<FINumOpaqueFloatingParams>(numFloatRegs),
                                false /*isInRegister*/,
                                spillOutput);
                }
                else
                {
                    return FastInterpBoilerplateLibrary<FIBitcastOpsImpl>::SelectBoilerplateBluePrint(
                                srcType,
                                dstType,
                                static_cast<FINumOpaqueIntegralParams>(numIntRegs - 1),
                                static_cast<FINumOpaqueFloatingParams>(numFloatRegs),
                                true /*isInRegister*/,
                                spillOutput);
                }
            };
        };

        result->InitArray(buildState, WasmOpcode::I32_BITCAST_F32, getRegisterFloatToIntBitcastFn(FastInterpTypeId::Get<float>(), FastInterpTypeId::Get<uint32_t>()));
        result->InitArray(buildState, WasmOpcode::I64_BITCAST_F64, getRegisterFloatToIntBitcastFn(FastInterpTypeId::Get<double>(), FastInterpTypeId::Get<uint64_t>()));
        result->InitArray(buildState, WasmOpcode::F32_BITCAST_I32, getRegisterIntToFloatBitcastFn(FastInterpTypeId::Get<uint32_t>(), FastInterpTypeId::Get<float>()));
        result->InitArray(buildState, WasmOpcode::F64_BITCAST_I64, getRegisterIntToFloatBitcastFn(FastInterpTypeId::Get<uint64_t>(), FastInterpTypeId::Get<double>()));

        auto getRegisterExtendOpsFn = [](FastInterpTypeId srcType, FastInterpTypeId dstType)
        {
            return [=](int numIntRegs, int /*numFloatRegs*/, bool spillOutput) -> const FastInterpBoilerplateBluePrint*
            {
                if (spillOutput && numIntRegs > 1)
                {
                    return nullptr;
                }
                if (numIntRegs == 0)
                {
                    return FastInterpBoilerplateLibrary<FIExtendOpsImpl>::SelectBoilerplateBluePrint(
                                srcType,
                                dstType,
                                static_cast<FINumOpaqueIntegralParams>(0),
                                FIOpaqueParamsHelper::GetMaxOFP(),
                                false /*isInRegister*/,
                                spillOutput);
                }
                else
                {
                    return FastInterpBoilerplateLibrary<FIExtendOpsImpl>::SelectBoilerplateBluePrint(
                                srcType,
                                dstType,
                                static_cast<FINumOpaqueIntegralParams>(numIntRegs - 1),
                                FIOpaqueParamsHelper::GetMaxOFP(),
                                true /*isInRegister*/,
                                spillOutput);
                }
            };
        };

        result->InitArray(buildState, WasmOpcode::I32_EXTEND_8S, getRegisterExtendOpsFn(FastInterpTypeId::Get<int8_t>(), FastInterpTypeId::Get<int32_t>()));
        result->InitArray(buildState, WasmOpcode::I32_EXTEND_16S, getRegisterExtendOpsFn(FastInterpTypeId::Get<int16_t>(), FastInterpTypeId::Get<int32_t>()));
        result->InitArray(buildState, WasmOpcode::I64_EXTEND_8S, getRegisterExtendOpsFn(FastInterpTypeId::Get<int8_t>(), FastInterpTypeId::Get<int64_t>()));
        result->InitArray(buildState, WasmOpcode::I64_EXTEND_16S, getRegisterExtendOpsFn(FastInterpTypeId::Get<int16_t>(), FastInterpTypeId::Get<int64_t>()));
        result->InitArray(buildState, WasmOpcode::I64_EXTEND_32S, getRegisterExtendOpsFn(FastInterpTypeId::Get<int32_t>(), FastInterpTypeId::Get<int64_t>()));

        auto registerSwtichSfFn = [](int /*numIntRegs*/, int /*numFloatRegs*/, bool /*spillOutput*/) -> const FastInterpBoilerplateBluePrint*
        {
            return FastInterpBoilerplateLibrary<FICallSwitchSfImpl>::SelectBoilerplateBluePrint(false);
        };
        result->InitArray(buildState, WasmOpcode::XX_SWITCH_SF, registerSwtichSfFn);

        auto getRegisterFillIntParamFn = [](FastInterpTypeId typeId)
        {
            return [=](int numIntRegs, int /*numFloatRegs*/, bool /*spillOutput*/) -> const FastInterpBoilerplateBluePrint*
            {
                return FastInterpBoilerplateLibrary<FICallStoreIntParamImpl>::SelectBoilerplateBluePrint(
                            typeId,
                            static_cast<NumIntegralParamsAfterBlock>(numIntRegs),
                            static_cast<FINumOpaqueIntegralParams>(0),
                            FIOpaqueParamsHelper::GetMaxOFP());
            };
        };
        result->InitArray(buildState, WasmOpcode::XX_I32_FILLPARAM, getRegisterFillIntParamFn(FastInterpTypeId::Get<uint32_t>()));
        result->InitArray(buildState, WasmOpcode::XX_I64_FILLPARAM, getRegisterFillIntParamFn(FastInterpTypeId::Get<uint64_t>()));

        auto getRegisterFillFloatParamFn = [](FastInterpTypeId typeId)
        {
            return [=](int /*numIntRegs*/, int numFloatRegs, bool /*spillOutput*/) -> const FastInterpBoilerplateBluePrint*
            {
                return FastInterpBoilerplateLibrary<FICallStoreFloatParamImpl>::SelectBoilerplateBluePrint(
                            typeId,
                            FIOpaqueParamsHelper::GetMaxOIP(),
                            static_cast<FINumOpaqueFloatingParams>((numFloatRegs > 0) ? numFloatRegs - 1 : 0),
                            numFloatRegs > 0);
            };
        };
        result->InitArray(buildState, WasmOpcode::XX_F32_FILLPARAM, getRegisterFillFloatParamFn(FastInterpTypeId::Get<float>()));
        result->InitArray(buildState, WasmOpcode::XX_F64_FILLPARAM, getRegisterFillFloatParamFn(FastInterpTypeId::Get<double>()));

        auto getRegisterReturnFn = [](FastInterpTypeId typeId)
        {
            return [=](int numIntRegs, int numFloatRegs, bool /*spillOutput*/) -> const FastInterpBoilerplateBluePrint*
            {
                if (typeId.GetTypeId().IsFloatingPoint())
                {
                    return FastInterpBoilerplateLibrary<FIReturnOpsImpl>::SelectBoilerplateBluePrint(
                                typeId,
                                FIOpaqueParamsHelper::GetMaxOIP(),
                                static_cast<FINumOpaqueFloatingParams>((numFloatRegs > 0) ? numFloatRegs - 1 : 0),
                                numFloatRegs > 0 /*isInRegister*/);
                }
                else
                {
                    return FastInterpBoilerplateLibrary<FIReturnOpsImpl>::SelectBoilerplateBluePrint(
                                typeId,
                                static_cast<FINumOpaqueIntegralParams>((numIntRegs > 0) ? numIntRegs - 1 : 0),
                                FIOpaqueParamsHelper::GetMaxOFP(),
                                numIntRegs > 0 /*isInRegister*/);
                }
            };
        };

        result->InitArray(buildState, WasmOpcode::XX_I32_RETURN, getRegisterReturnFn(FastInterpTypeId::Get<uint32_t>()), true /*forReturn*/);
        result->InitArray(buildState, WasmOpcode::XX_I64_RETURN, getRegisterReturnFn(FastInterpTypeId::Get<uint64_t>()), true /*forReturn*/);
        result->InitArray(buildState, WasmOpcode::XX_F32_RETURN, getRegisterReturnFn(FastInterpTypeId::Get<float>()), true /*forReturn*/);
        result->InitArray(buildState, WasmOpcode::XX_F64_RETURN, getRegisterReturnFn(FastInterpTypeId::Get<double>()), true /*forReturn*/);

        auto registerReturnNoneFn = [](int /*numIntRegs*/, int /*numFloatRegs*/, bool /*spillOutput*/) -> const FastInterpBoilerplateBluePrint*
        {
            return FastInterpBoilerplateLibrary<FIReturnNoneImpl>::SelectBoilerplateBluePrint(false /*dummy*/);
        };
        result->InitArray(buildState, WasmOpcode::XX_NONE_RETURN, registerReturnNoneFn, true /*forReturn*/);

        auto registerDropFn = [](int /*numIntRegs*/, int /*numFloatRegs*/, bool /*spillOutput*/) -> const FastInterpBoilerplateBluePrint*
        {
            return FastInterpBoilerplateLibrary<FINoopImpl>::SelectBoilerplateBluePrint(
                        FIOpaqueParamsHelper::GetMaxOIP(),
                        FIOpaqueParamsHelper::GetMaxOFP());
        };

        result->InitArray(buildState, WasmOpcode::XX_I_DROP, registerDropFn);
        result->InitArray(buildState, WasmOpcode::XX_F_DROP, registerDropFn);

        auto getRegisterSelectIntFn = [](FastInterpTypeId typeId)
        {
            return [=](int numIntRegs, int /*numFloatRegs*/, bool spillOutput) -> const FastInterpBoilerplateBluePrint*
            {
                if (spillOutput && numIntRegs > 3)
                {
                    return nullptr;
                }
                if (numIntRegs < 3)
                {
                    return FastInterpBoilerplateLibrary<FISelectIntImpl>::SelectBoilerplateBluePrint(
                                typeId,
                                static_cast<FINumOpaqueIntegralParams>(0),
                                FIOpaqueParamsHelper::GetMaxOFP(),
                                static_cast<TrinaryOpNumInRegisterOperands>(numIntRegs),
                                spillOutput);
                }
                else
                {
                    return FastInterpBoilerplateLibrary<FISelectIntImpl>::SelectBoilerplateBluePrint(
                                typeId,
                                static_cast<FINumOpaqueIntegralParams>(numIntRegs - 3),
                                FIOpaqueParamsHelper::GetMaxOFP(),
                                TrinaryOpNumInRegisterOperands::THREE,
                                spillOutput);
                }
            };
        };

        result->InitArray(buildState, WasmOpcode::XX_I32_SELECT, getRegisterSelectIntFn(FastInterpTypeId::Get<uint32_t>()));
        result->InitArray(buildState, WasmOpcode::XX_I64_SELECT, getRegisterSelectIntFn(FastInterpTypeId::Get<uint64_t>()));

        auto getRegisterSelectFloatFn = [](FastInterpTypeId typeId)
        {
            return [=](int numIntRegs, int numFloatRegs, bool spillOutput) -> const FastInterpBoilerplateBluePrint*
            {
                if (spillOutput && numFloatRegs > 2)
                {
                    return nullptr;
                }
                if (numFloatRegs >= 2)
                {
                    return FastInterpBoilerplateLibrary<FISelectFloatImpl>::SelectBoilerplateBluePrint(
                                typeId,
                                static_cast<FINumOpaqueIntegralParams>(numIntRegs == 0 ? 0 : numIntRegs - 1),
                                static_cast<FINumOpaqueFloatingParams>(numFloatRegs - 2),
                                NumInRegisterOperands::TWO,
                                numIntRegs == 0 /*isSelectorSpilled*/,
                                spillOutput);
                }
                else
                {
                    return FastInterpBoilerplateLibrary<FISelectFloatImpl>::SelectBoilerplateBluePrint(
                                typeId,
                                static_cast<FINumOpaqueIntegralParams>(numIntRegs == 0 ? 0 : numIntRegs - 1),
                                static_cast<FINumOpaqueFloatingParams>(0),
                                static_cast<NumInRegisterOperands>(numFloatRegs),
                                numIntRegs == 0 /*isSelectorSpilled*/,
                                spillOutput);
                }
            };
        };

        result->InitArray(buildState, WasmOpcode::XX_F32_SELECT, getRegisterSelectFloatFn(FastInterpTypeId::Get<float>()));
        result->InitArray(buildState, WasmOpcode::XX_F64_SELECT, getRegisterSelectFloatFn(FastInterpTypeId::Get<double>()));

        auto getRegisterLocalGetFn = [](FastInterpTypeId typeId)
        {
            return [=](int numIntRegs, int numFloatRegs, bool spillOutput) -> const FastInterpBoilerplateBluePrint*
            {
                if (typeId.GetTypeId().IsFloatingPoint())
                {
                    if (numFloatRegs > 0 && spillOutput)
                    {
                        return nullptr;
                    }
                    return FastInterpBoilerplateLibrary<FILocalGetImpl>::SelectBoilerplateBluePrint(
                                typeId,
                                FIOpaqueParamsHelper::GetMaxOIP(),
                                static_cast<FINumOpaqueFloatingParams>(numFloatRegs),
                                spillOutput);
                }
                else
                {
                    if (numIntRegs > 0 && spillOutput)
                    {
                        return nullptr;
                    }
                    return FastInterpBoilerplateLibrary<FILocalGetImpl>::SelectBoilerplateBluePrint(
                                typeId,
                                static_cast<FINumOpaqueIntegralParams>(numIntRegs),
                                FIOpaqueParamsHelper::GetMaxOFP(),
                                spillOutput);
                }
            };
        };

        result->InitArray(buildState, WasmOpcode::XX_I32_LOCAL_GET, getRegisterLocalGetFn(FastInterpTypeId::Get<uint32_t>()));
        result->InitArray(buildState, WasmOpcode::XX_I64_LOCAL_GET, getRegisterLocalGetFn(FastInterpTypeId::Get<uint64_t>()));
        result->InitArray(buildState, WasmOpcode::XX_F32_LOCAL_GET, getRegisterLocalGetFn(FastInterpTypeId::Get<float>()));
        result->InitArray(buildState, WasmOpcode::XX_F64_LOCAL_GET, getRegisterLocalGetFn(FastInterpTypeId::Get<double>()));

        auto getRegisterLocalSetFn = [](FastInterpTypeId typeId)
        {
            return [=](int numIntRegs, int numFloatRegs, bool /*spillOutput*/) -> const FastInterpBoilerplateBluePrint*
            {
                if (typeId.GetTypeId().IsFloatingPoint())
                {
                    if (numFloatRegs == 0)
                    {
                        return FastInterpBoilerplateLibrary<FILocalStoreOrTeeImpl>::SelectBoilerplateBluePrint(
                                    typeId,
                                    FIOpaqueParamsHelper::GetMaxOIP(),
                                    static_cast<FINumOpaqueFloatingParams>(0),
                                    false /*isInRegister*/,
                                    false /*isTee*/,
                                    false /*spillOutput*/);
                    }
                    else
                    {
                        return FastInterpBoilerplateLibrary<FILocalStoreOrTeeImpl>::SelectBoilerplateBluePrint(
                                    typeId,
                                    FIOpaqueParamsHelper::GetMaxOIP(),
                                    static_cast<FINumOpaqueFloatingParams>(numFloatRegs - 1),
                                    true /*isInRegister*/,
                                    false /*isTee*/,
                                    false /*spillOutput*/);
                    }
                }
                else
                {
                    if (numIntRegs == 0)
                    {
                        return FastInterpBoilerplateLibrary<FILocalStoreOrTeeImpl>::SelectBoilerplateBluePrint(
                                    typeId,
                                    static_cast<FINumOpaqueIntegralParams>(0),
                                    FIOpaqueParamsHelper::GetMaxOFP(),
                                    false /*isInRegister*/,
                                    false /*isTee*/,
                                    false /*spillOutput*/);
                    }
                    else
                    {
                        return FastInterpBoilerplateLibrary<FILocalStoreOrTeeImpl>::SelectBoilerplateBluePrint(
                                    typeId,
                                    static_cast<FINumOpaqueIntegralParams>(numIntRegs - 1),
                                    FIOpaqueParamsHelper::GetMaxOFP(),
                                    true /*isInRegister*/,
                                    false /*isTee*/,
                                    false /*spillOutput*/);
                    }
                }
            };
        };

        result->InitArray(buildState, WasmOpcode::XX_I32_LOCAL_SET, getRegisterLocalSetFn(FastInterpTypeId::Get<uint32_t>()));
        result->InitArray(buildState, WasmOpcode::XX_I64_LOCAL_SET, getRegisterLocalSetFn(FastInterpTypeId::Get<uint64_t>()));
        result->InitArray(buildState, WasmOpcode::XX_F32_LOCAL_SET, getRegisterLocalSetFn(FastInterpTypeId::Get<float>()));
        result->InitArray(buildState, WasmOpcode::XX_F64_LOCAL_SET, getRegisterLocalSetFn(FastInterpTypeId::Get<double>()));

        auto getRegisterLocalTeeFn = [](FastInterpTypeId typeId)
        {
            return [=](int numIntRegs, int numFloatRegs, bool spillOutput) -> const FastInterpBoilerplateBluePrint*
            {
                if (typeId.GetTypeId().IsFloatingPoint())
                {
                    if (spillOutput && numFloatRegs > 1)
                    {
                        return nullptr;
                    }
                    if (numFloatRegs == 0)
                    {
                        return FastInterpBoilerplateLibrary<FILocalStoreOrTeeImpl>::SelectBoilerplateBluePrint(
                                    typeId,
                                    FIOpaqueParamsHelper::GetMaxOIP(),
                                    static_cast<FINumOpaqueFloatingParams>(0),
                                    false /*isInRegister*/,
                                    true /*isTee*/,
                                    spillOutput);
                    }
                    else
                    {
                        return FastInterpBoilerplateLibrary<FILocalStoreOrTeeImpl>::SelectBoilerplateBluePrint(
                                    typeId,
                                    FIOpaqueParamsHelper::GetMaxOIP(),
                                    static_cast<FINumOpaqueFloatingParams>(numFloatRegs - 1),
                                    true /*isInRegister*/,
                                    true /*isTee*/,
                                    spillOutput);
                    }
                }
                else
                {
                    if (spillOutput && numIntRegs > 1)
                    {
                        return nullptr;
                    }
                    if (numIntRegs == 0)
                    {
                        return FastInterpBoilerplateLibrary<FILocalStoreOrTeeImpl>::SelectBoilerplateBluePrint(
                                    typeId,
                                    static_cast<FINumOpaqueIntegralParams>(0),
                                    FIOpaqueParamsHelper::GetMaxOFP(),
                                    false /*isInRegister*/,
                                    true /*isTee*/,
                                    spillOutput);
                    }
                    else
                    {
                        return FastInterpBoilerplateLibrary<FILocalStoreOrTeeImpl>::SelectBoilerplateBluePrint(
                                    typeId,
                                    static_cast<FINumOpaqueIntegralParams>(numIntRegs - 1),
                                    FIOpaqueParamsHelper::GetMaxOFP(),
                                    true /*isInRegister*/,
                                    true /*isTee*/,
                                    spillOutput);
                    }
                }
            };
        };

        result->InitArray(buildState, WasmOpcode::XX_I32_LOCAL_TEE, getRegisterLocalTeeFn(FastInterpTypeId::Get<uint32_t>()));
        result->InitArray(buildState, WasmOpcode::XX_I64_LOCAL_TEE, getRegisterLocalTeeFn(FastInterpTypeId::Get<uint64_t>()));
        result->InitArray(buildState, WasmOpcode::XX_F32_LOCAL_TEE, getRegisterLocalTeeFn(FastInterpTypeId::Get<float>()));
        result->InitArray(buildState, WasmOpcode::XX_F64_LOCAL_TEE, getRegisterLocalTeeFn(FastInterpTypeId::Get<double>()));

        auto getRegisterGlobalGetFn = [](FastInterpTypeId typeId)
        {
            return [=](int numIntRegs, int numFloatRegs, bool spillOutput) -> const FastInterpBoilerplateBluePrint*
            {
                if (typeId.GetTypeId().IsFloatingPoint())
                {
                    if (spillOutput && numFloatRegs > 0)
                    {
                        return nullptr;
                    }
                    if (numFloatRegs == x_maxFloatRegs && !spillOutput)
                    {
                        return nullptr;
                    }
                    return FastInterpBoilerplateLibrary<FIGlobalGetImpl>::SelectBoilerplateBluePrint(
                                typeId,
                                FIOpaqueParamsHelper::GetMaxOIP(),
                                static_cast<FINumOpaqueFloatingParams>(numFloatRegs),
                                spillOutput);
                }
                else
                {
                    if (spillOutput && numIntRegs > 0)
                    {
                        return nullptr;
                    }
                    if (numIntRegs == x_maxIntRegs && !spillOutput)
                    {
                        return nullptr;
                    }
                    return FastInterpBoilerplateLibrary<FIGlobalGetImpl>::SelectBoilerplateBluePrint(
                                typeId,
                                static_cast<FINumOpaqueIntegralParams>(numIntRegs),
                                FIOpaqueParamsHelper::GetMaxOFP(),
                                spillOutput);
                }
            };
        };

        result->InitArray(buildState, WasmOpcode::XX_I32_GLOBAL_GET, getRegisterGlobalGetFn(FastInterpTypeId::Get<uint32_t>()));
        result->InitArray(buildState, WasmOpcode::XX_I64_GLOBAL_GET, getRegisterGlobalGetFn(FastInterpTypeId::Get<uint64_t>()));
        result->InitArray(buildState, WasmOpcode::XX_F32_GLOBAL_GET, getRegisterGlobalGetFn(FastInterpTypeId::Get<float>()));
        result->InitArray(buildState, WasmOpcode::XX_F64_GLOBAL_GET, getRegisterGlobalGetFn(FastInterpTypeId::Get<double>()));

        auto getRegisterGlobalSetFn = [](FastInterpTypeId typeId)
        {
            return [=](int numIntRegs, int numFloatRegs, bool /*spillOutput*/) -> const FastInterpBoilerplateBluePrint*
            {
                if (typeId.GetTypeId().IsFloatingPoint())
                {
                    if (numFloatRegs == 0)
                    {
                        return FastInterpBoilerplateLibrary<FIGlobalSetImpl>::SelectBoilerplateBluePrint(
                                    typeId,
                                    FIOpaqueParamsHelper::GetMaxOIP(),
                                    static_cast<FINumOpaqueFloatingParams>(0),
                                    false /*isInRegister*/);
                    }
                    else
                    {
                        return FastInterpBoilerplateLibrary<FIGlobalSetImpl>::SelectBoilerplateBluePrint(
                                    typeId,
                                    FIOpaqueParamsHelper::GetMaxOIP(),
                                    static_cast<FINumOpaqueFloatingParams>(numFloatRegs - 1),
                                    true /*isInRegister*/);
                    }
                }
                else
                {
                    if (numIntRegs == 0)
                    {
                        return FastInterpBoilerplateLibrary<FIGlobalSetImpl>::SelectBoilerplateBluePrint(
                                    typeId,
                                    static_cast<FINumOpaqueIntegralParams>(0),
                                    FIOpaqueParamsHelper::GetMaxOFP(),
                                    false /*isInRegister*/);
                    }
                    else
                    {
                        return FastInterpBoilerplateLibrary<FIGlobalSetImpl>::SelectBoilerplateBluePrint(
                                    typeId,
                                    static_cast<FINumOpaqueIntegralParams>(numIntRegs - 1),
                                    FIOpaqueParamsHelper::GetMaxOFP(),
                                    true /*isInRegister*/);
                    }
                }
            };
        };

        result->InitArray(buildState, WasmOpcode::XX_I32_GLOBAL_SET, getRegisterGlobalSetFn(FastInterpTypeId::Get<uint32_t>()));
        result->InitArray(buildState, WasmOpcode::XX_I64_GLOBAL_SET, getRegisterGlobalSetFn(FastInterpTypeId::Get<uint64_t>()));
        result->InitArray(buildState, WasmOpcode::XX_F32_GLOBAL_SET, getRegisterGlobalSetFn(FastInterpTypeId::Get<float>()));
        result->InitArray(buildState, WasmOpcode::XX_F64_GLOBAL_SET, getRegisterGlobalSetFn(FastInterpTypeId::Get<double>()));

        ReleaseAssert(buildState.m_curAddr <= curAddrLimit);

        for (uint32_t op = 0; op <= 255; op++)
        {
            result->m_maxSize[op] = 0;
            for (uint32_t i = 0; i <= x_maxIntRegs; i++)
            {
                for (uint32_t j = 0; j <= x_maxFloatRegs; j++)
                {
                    for (bool k : {false, true})
                    {
                        OffsetType offset = result->m_array[op][i][j][k];
                        if (offset != static_cast<OffsetType>(-1))
                        {
                            WasmCommonOpcodeStencil* s = reinterpret_cast<WasmCommonOpcodeStencil*>(reinterpret_cast<uintptr_t>(result) + offset);
                            result->m_maxSize[op] = std::max(result->m_maxSize[op], s->m_contentLenBytes);
                        }
                    }
                }
            }
        }
        return result;
    }

    WasmCommonOpcodeStencil* Get(WasmOpcode opcode, uint32_t numIntRegs, uint32_t numFloatRegs, bool spillOutput)
    {
        assert(numIntRegs <= x_maxIntRegs && numFloatRegs <= x_maxFloatRegs);
        OffsetType offset = m_array[static_cast<uint32_t>(opcode)][numIntRegs][numFloatRegs][spillOutput];
        assert(offset != static_cast<OffsetType>(-1));
        return reinterpret_cast<WasmCommonOpcodeStencil*>(reinterpret_cast<uintptr_t>(this) + offset);
    }

    uint8_t GetMaxSizeForOpcode(WasmOpcode opcode)
    {
        return m_maxSize[static_cast<uint8_t>(opcode)];
    }

private:
    using OffsetType = uint16_t;

    void InitArray(BuildState& buildState, WasmOpcode opcode, const std::function<const FastInterpBoilerplateBluePrint*(int, int, bool)>& fn, bool forReturn = false)
    {
        for (int numIntRegs = 0; numIntRegs <= x_maxIntRegs; numIntRegs++)
        {
            for (int numFloatRegs = 0; numFloatRegs <= x_maxFloatRegs; numFloatRegs++)
            {
                for (int spillOutput = 0; spillOutput <= 1; spillOutput++)
                {
                    ReleaseAssert(m_array[static_cast<uint32_t>(opcode)][numIntRegs][numFloatRegs][spillOutput] == static_cast<OffsetType>(-1));
                    const FastInterpBoilerplateBluePrint* blueprint = fn(numIntRegs, numFloatRegs, spillOutput);
                    if (blueprint == nullptr)
                    {
                        m_array[static_cast<uint32_t>(opcode)][numIntRegs][numFloatRegs][spillOutput] = static_cast<OffsetType>(-1);
                    }
                    else
                    {
                        if (buildState.m_cache.count(blueprint))
                        {
                            ssize_t offset = buildState.m_cache[blueprint] - reinterpret_cast<uint8_t*>(this);
                            ReleaseAssert(0 <= offset && static_cast<size_t>(offset) < std::numeric_limits<OffsetType>::max());
                            m_array[static_cast<uint32_t>(opcode)][numIntRegs][numFloatRegs][spillOutput] = static_cast<OffsetType>(offset);
                        }
                        else
                        {
                            buildState.m_cache[blueprint] = buildState.m_curAddr;
                            ssize_t offset = buildState.m_cache[blueprint] - reinterpret_cast<uint8_t*>(this);
                            ReleaseAssert(0 <= offset && static_cast<size_t>(offset) < std::numeric_limits<OffsetType>::max());
                            m_array[static_cast<uint32_t>(opcode)][numIntRegs][numFloatRegs][spillOutput] = static_cast<OffsetType>(offset);
                            if (!forReturn)
                            {
                                InitOpcodeStencil(buildState, blueprint);
                            }
                            else
                            {
                                InitOpcodeStencilForReturn(buildState, blueprint);
                            }
                        }
                    }
                }
            }
        }
    }

    void InitOpcodeStencilForReturn(BuildState& buildState, const FastInterpBoilerplateBluePrint* blueprint)
    {
        ReleaseAssert(blueprint->m_addr32FixupArrayLength == 0);
        ReleaseAssert(blueprint->m_symbol64FixupArrayLength == 0);
        ReleaseAssert(blueprint->m_highestCppFnptrPlaceholderOrdinal == 0);
        ReleaseAssert(blueprint->m_highestBoilerplateFnptrPlaceholderOrdinal == 0);

        WasmCommonOpcodeStencil* s = reinterpret_cast<WasmCommonOpcodeStencil*>(buildState.m_curAddr);
        buildState.m_curAddr += sizeof(WasmCommonOpcodeStencil);

        {
            uint64_t r = blueprint->m_contentLength;
            ReleaseAssert(r <= 255);
            s->m_contentLenBytes = static_cast<uint8_t>(r);
            memcpy(buildState.m_curAddr, blueprint->m_content, r);
        }

        std::vector<std::pair<uint8_t, uint8_t>> sym32vec;
        for (uint32_t i = 0; i < blueprint->m_symbol32FixupArrayLength; i++)
        {
            FastInterpSymbolFixupRecord record = blueprint->m_symbol32FixupArray[i];
            ReleaseAssert(record.m_offset + sizeof(uint32_t) <= s->m_contentLenBytes);
            uint32_t dataOrd = record.m_ordinalIntoPlaceholderArray;
            ReleaseAssert(dataOrd <= 2 || (8 <= dataOrd && dataOrd <= 12));
            if (8 <= dataOrd && dataOrd <= 12)
            {
                if (dataOrd == 8)
                {
                    dataOrd = 0;
                    UnalignedAddAndWriteback<uint32_t>(buildState.m_curAddr + record.m_offset, 8);
                }
                else if (dataOrd == 9)
                {
                    dataOrd = 0;
                    UnalignedAddAndWriteback<uint32_t>(buildState.m_curAddr + record.m_offset, static_cast<uint32_t>(-8));
                }
                else if (dataOrd == 10)
                {
                    dataOrd = 0;
                    UnalignedAddAndWriteback<uint32_t>(buildState.m_curAddr + record.m_offset, static_cast<uint32_t>(-16));
                }
                else if (dataOrd == 11)
                {
                    dataOrd = 1;
                    UnalignedAddAndWriteback<uint32_t>(buildState.m_curAddr + record.m_offset, 8);
                }
                else
                {
                    ReleaseAssert(dataOrd == 12);
                    dataOrd = 1;
                    UnalignedAddAndWriteback<uint32_t>(buildState.m_curAddr + record.m_offset, static_cast<uint32_t>(-8));
                }
            }
            ReleaseAssert(0 <= dataOrd && dataOrd <= 2);
            ReleaseAssert(record.m_offset <= 255);
            sym32vec.push_back(std::make_pair(static_cast<uint8_t>(dataOrd), static_cast<uint8_t>(record.m_offset)));
        }
        std::sort(sym32vec.begin(), sym32vec.end());

        buildState.m_curAddr += s->m_contentLenBytes;
        {
            uint64_t r = sym32vec.size() * 2;
            ReleaseAssert(r <= 255);
            s->m_sym32FixupArrayLenBytes = static_cast<uint8_t>(r);
            for (uint64_t i = 0; i < sym32vec.size(); i++)
            {
                *buildState.m_curAddr = sym32vec[i].first;
                buildState.m_curAddr++;
                *buildState.m_curAddr = sym32vec[i].second;
                buildState.m_curAddr++;
            }
        }

        s->m_sym64FixupArrayLenBytes = 0;
    }

    void InitOpcodeStencil(BuildState& buildState, const FastInterpBoilerplateBluePrint* blueprint)
    {
        ReleaseAssert(blueprint->m_addr32FixupArrayLength > 0 &&
                      blueprint->m_addr32FixupArray[blueprint->m_addr32FixupArrayLength - 1] == blueprint->m_contentLength - 4);
        ReleaseAssert(blueprint->m_symbol32FixupArrayLength > 0 &&
                      blueprint->m_symbol32FixupArray[blueprint->m_symbol32FixupArrayLength - 1].m_offset == blueprint->m_contentLength - 4);
        ReleaseAssert(blueprint->m_highestCppFnptrPlaceholderOrdinal == 0);
        ReleaseAssert(blueprint->m_highestBoilerplateFnptrPlaceholderOrdinal == 1);

        WasmCommonOpcodeStencil* s = reinterpret_cast<WasmCommonOpcodeStencil*>(buildState.m_curAddr);
        buildState.m_curAddr += sizeof(WasmCommonOpcodeStencil);

        {
            uint64_t r = blueprint->m_contentLength - x86_64_rip_relative_jmp_instruction_len;
            ReleaseAssert(r <= 255);
            s->m_contentLenBytes = static_cast<uint8_t>(r);
            memcpy(buildState.m_curAddr, blueprint->m_content, r);
        }

        for (uint32_t i = 0; i < blueprint->m_addr32FixupArrayLength - 1; i++)
        {
            uint32_t offset = blueprint->m_addr32FixupArray[i];
            ReleaseAssert(offset + sizeof(uint32_t) <= s->m_contentLenBytes);
            uint32_t fixup = static_cast<uint32_t>(s->m_contentLenBytes);
            UnalignedAddAndWriteback<uint32_t>(buildState.m_curAddr + offset, fixup);
        }

        std::vector<std::pair<uint8_t, uint8_t>> sym32vec;
        for (uint32_t i = 0; i < blueprint->m_symbol32FixupArrayLength - 1; i++)
        {
            FastInterpSymbolFixupRecord record = blueprint->m_symbol32FixupArray[i];
            if (record.m_ordinalIntoPlaceholderArray == 0) { continue; }
            ReleaseAssert(record.m_offset + sizeof(uint32_t) <= s->m_contentLenBytes);
            uint32_t dataOrd = record.m_ordinalIntoPlaceholderArray - 1;
            ReleaseAssert(dataOrd <= 2 || (8 <= dataOrd && dataOrd <= 12));
            if (8 <= dataOrd && dataOrd <= 12)
            {
                if (dataOrd == 8)
                {
                    dataOrd = 0;
                    UnalignedAddAndWriteback<uint32_t>(buildState.m_curAddr + record.m_offset, 8);
                }
                else if (dataOrd == 9)
                {
                    dataOrd = 0;
                    UnalignedAddAndWriteback<uint32_t>(buildState.m_curAddr + record.m_offset, static_cast<uint32_t>(-8));
                }
                else if (dataOrd == 10)
                {
                    dataOrd = 0;
                    UnalignedAddAndWriteback<uint32_t>(buildState.m_curAddr + record.m_offset, static_cast<uint32_t>(-16));
                }
                else if (dataOrd == 11)
                {
                    dataOrd = 1;
                    UnalignedAddAndWriteback<uint32_t>(buildState.m_curAddr + record.m_offset, 8);
                }
                else
                {
                    ReleaseAssert(dataOrd == 12);
                    dataOrd = 1;
                    UnalignedAddAndWriteback<uint32_t>(buildState.m_curAddr + record.m_offset, static_cast<uint32_t>(-8));
                }
            }
            ReleaseAssert(0 <= dataOrd && dataOrd <= 2);
            ReleaseAssert(record.m_offset <= 255);
            sym32vec.push_back(std::make_pair(static_cast<uint8_t>(dataOrd), static_cast<uint8_t>(record.m_offset)));
        }
        std::sort(sym32vec.begin(), sym32vec.end());

        uint8_t* codeBegin = buildState.m_curAddr;
        buildState.m_curAddr += s->m_contentLenBytes;
        {
            uint64_t r = sym32vec.size() * 2;
            ReleaseAssert(r <= 255);
            s->m_sym32FixupArrayLenBytes = static_cast<uint8_t>(r);
            for (uint64_t i = 0; i < sym32vec.size(); i++)
            {
                *buildState.m_curAddr = sym32vec[i].first;
                buildState.m_curAddr++;
                *buildState.m_curAddr = sym32vec[i].second;
                buildState.m_curAddr++;
            }
        }

        s->m_sym64FixupArrayLenBytes = static_cast<uint8_t>(blueprint->m_symbol64FixupArrayLength * 2);
        for (uint32_t i = 0; i < blueprint->m_symbol64FixupArrayLength; i++)
        {
            ReleaseAssert(blueprint->m_symbol64FixupArray[i].m_offset + sizeof(uint64_t) <= s->m_contentLenBytes);
            uint32_t offset = blueprint->m_symbol64FixupArray[i].m_offset;
            uint32_t dataOrd = blueprint->m_symbol64FixupArray[i].m_ordinalIntoPlaceholderArray - 1;
            ReleaseAssert(dataOrd <= 2 || (8 <= dataOrd && dataOrd <= 12));
            if (8 <= dataOrd && dataOrd <= 12)
            {
                if (dataOrd == 8)
                {
                    dataOrd = 0;
                    UnalignedAddAndWriteback<uint64_t>(codeBegin + offset, 8);
                }
                else if (dataOrd == 9)
                {
                    dataOrd = 0;
                    UnalignedAddAndWriteback<uint32_t>(codeBegin + offset, static_cast<uint32_t>(-8));
                }
                else if (dataOrd == 10)
                {
                    dataOrd = 0;
                    UnalignedAddAndWriteback<uint32_t>(codeBegin + offset, static_cast<uint32_t>(-16));
                }
                else if (dataOrd == 11)
                {
                    dataOrd = 1;
                    UnalignedAddAndWriteback<uint32_t>(codeBegin + offset, 8);
                }
                else
                {
                    ReleaseAssert(dataOrd == 12);
                    dataOrd = 1;
                    UnalignedAddAndWriteback<uint32_t>(codeBegin + offset, static_cast<uint32_t>(-8));
                }
            }
            ReleaseAssert(0 <= dataOrd && dataOrd <= 2);
            *buildState.m_curAddr = static_cast<uint8_t>(dataOrd);
            buildState.m_curAddr++;
            *buildState.m_curAddr = static_cast<uint8_t>(offset);
            buildState.m_curAddr++;
        }
    }

    OffsetType m_array[256][x_maxIntRegs + 1][x_maxFloatRegs + 1][2];
    uint8_t m_maxSize[256];
};

extern WasmCommonOpcodeManager* g_wasmCommonOpcodeManager;
WasmCommonOpcodeManager* g_wasmCommonOpcodeManager = WasmCommonOpcodeManager::Build();

struct WasmBranchOpcodeStencil
{
    uint8_t m_contentLenBytes;
    uint8_t m_sym32FixupArrayLenBytes;
    uint8_t m_targetSlotOffset;

    const uint8_t* GetContentStart() const
    {
        return reinterpret_cast<const uint8_t*>(this + 1);
    }

    const uint8_t* GetFixupArraysStart() const
    {
        return GetContentStart() + m_contentLenBytes;
    }

    uint8_t* WARN_UNUSED Fixup(uint8_t*& destAddr, WasmCommonOpcodeFixups* input) const
    {
        memcpy(destAddr, GetContentStart(), m_contentLenBytes);
        const uint8_t* cur = GetFixupArraysStart();
        const uint8_t* sym32End = cur + m_sym32FixupArrayLenBytes;

        while (cur < sym32End)
        {
            uint8_t ord = *cur++;
            uint8_t offset = *cur++;
            assert(ord < 3 && offset + sizeof(uint32_t) <= m_contentLenBytes);
            uint32_t addend = static_cast<uint32_t>(input->m_data[ord]);
            UnalignedAddAndWriteback<uint32_t>(destAddr + offset, addend);
        }

        uint8_t* result = destAddr + m_targetSlotOffset;
        UnalignedAddAndWriteback<uint32_t>(result, static_cast<uint32_t>(-reinterpret_cast<uint64_t>(destAddr)));

        destAddr += m_contentLenBytes;
        return result;
    }
};

class WasmBranchManager
{
public:
    using OffsetType = uint16_t;

    // returns the address to populate the target address
    // generates something like this:
    //   ... cmp ...
    //   je8 not_taken
    //   store result to appropriate place if needed
    //   jmp [target]
    // not_taken: ..
    //
    uint8_t* WARN_UNUSED CodegenCondBranchWithOutput(uint8_t*& dstAddr,
                                                     uint32_t oldNumInRegInt, uint32_t oldNumInRegFloat,
                                                     uint32_t newNumInRegInt, uint32_t newNumInRegFloat,
                                                     WasmValueType outputType,
                                                     bool spillOutput,
                                                     WasmCommonOpcodeFixups* fixups)
    {
        assert(0 <= oldNumInRegInt && oldNumInRegInt <= x_maxIntRegs);
        assert(0 <= newNumInRegInt && newNumInRegInt <= x_maxIntRegs);
        assert(0 <= oldNumInRegFloat && oldNumInRegFloat <= x_maxFloatRegs);
        assert(0 <= newNumInRegFloat && newNumInRegFloat <= x_maxFloatRegs);
        assert(outputType != WasmValueType::X_END_OF_ENUM);
        OffsetType r;
        if (WasmValueTypeHelper::IsIntegral(outputType))
        {
            r = m_condBrWithIntOutput[outputType == WasmValueType::I32][oldNumInRegInt][newNumInRegInt][spillOutput];
        }
        else
        {
            r = m_condBrWithFloatOutput[outputType == WasmValueType::F32][oldNumInRegInt][oldNumInRegFloat][newNumInRegFloat][spillOutput];
        }
        assert(r != static_cast<OffsetType>(-1));
        WasmBranchOpcodeStencil* s = reinterpret_cast<WasmBranchOpcodeStencil*>(reinterpret_cast<uintptr_t>(this) + r);
        return s->Fixup(dstAddr, fixups);
    }

    // returns the address to populate the target address
    // generates something like this:
    //   ... cmp ...
    //   je8 not_taken
    //   jmp [target]
    // not_taken: ..
    //
    uint8_t* WARN_UNUSED CodegenCondBranchWithoutOutput(uint8_t*& dstAddr,
                                                        uint32_t numInRegInt,
                                                        WasmCommonOpcodeFixups* fixups)
    {
        assert(0 <= numInRegInt && numInRegInt <= x_maxIntRegs);
        OffsetType r = m_condBrWithoutOutput[numInRegInt];
        assert(r != static_cast<OffsetType>(-1));
        WasmBranchOpcodeStencil* s = reinterpret_cast<WasmBranchOpcodeStencil*>(reinterpret_cast<uintptr_t>(this) + r);
        return s->Fixup(dstAddr, fixups);
    }

    // returns the address to populate the target address
    // generates something like this:
    //   store result to appropriate place if needed
    //   jmp [target]
    //
    uint8_t* WARN_UNUSED CodegenBranchWithOutput(uint8_t*& dstAddr,
                                                 uint32_t oldNumInRegInt, uint32_t oldNumInRegFloat,
                                                 uint32_t newNumInRegInt, uint32_t newNumInRegFloat,
                                                 WasmValueType outputType,
                                                 bool spillOutput,
                                                 WasmCommonOpcodeFixups* fixups)
    {
        assert(0 <= oldNumInRegInt && oldNumInRegInt <= x_maxIntRegs);
        assert(0 <= newNumInRegInt && newNumInRegInt <= x_maxIntRegs);
        assert(0 <= oldNumInRegFloat && oldNumInRegFloat <= x_maxFloatRegs);
        assert(0 <= newNumInRegFloat && newNumInRegFloat <= x_maxFloatRegs);
        assert(outputType != WasmValueType::X_END_OF_ENUM);
        OffsetType r;
        if (WasmValueTypeHelper::IsIntegral(outputType))
        {
            r = m_brWithIntOutput[outputType == WasmValueType::I32][oldNumInRegInt][newNumInRegInt][spillOutput];
        }
        else
        {
            r = m_brWithFloatOutput[outputType == WasmValueType::F32][oldNumInRegFloat][newNumInRegFloat][spillOutput];
        }
        assert(r != static_cast<OffsetType>(-1));
        WasmBranchOpcodeStencil* s = reinterpret_cast<WasmBranchOpcodeStencil*>(reinterpret_cast<uintptr_t>(this) + r);
        return s->Fixup(dstAddr, fixups);
    }


    // returns the address to populate the target address
    // generates something like this:
    //   jmp [target]
    //
    uint8_t* WARN_UNUSED CodegenBranchWithoutOutput(uint8_t*& dstAddr)
    {
        *dstAddr = x86_64_jmp_instruction_opcode;
        UnalignedWrite<uint32_t>(dstAddr + 1, -static_cast<uint32_t>(reinterpret_cast<uint64_t>(dstAddr + 5)));
        dstAddr += 5;
        return dstAddr - 4;
    }

    // returns the address to populate the false branch address
    // generates something like this:
    //  ... cmp ...
    //  je32 not_taken
    //
    uint8_t* WARN_UNUSED CodegenIfBranch(uint8_t*& dstAddr,
                                         uint32_t numInRegInt,
                                         WasmCommonOpcodeFixups* fixups)
    {
        assert(0 <= numInRegInt && numInRegInt <= x_maxIntRegs);
        OffsetType r = m_ifBranch[numInRegInt];
        assert(r != static_cast<OffsetType>(-1));
        WasmBranchOpcodeStencil* s = reinterpret_cast<WasmBranchOpcodeStencil*>(reinterpret_cast<uintptr_t>(this) + r);
        return s->Fixup(dstAddr, fixups);
    }

    static WasmBranchManager* WARN_UNUSED Build()
    {
        constexpr uint32_t len = 32768;
        WasmBranchManager* result;
        {
            void* addr = mmap(nullptr, len + sizeof(WasmBranchManager), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
            if (addr == MAP_FAILED)
            {
                ReleaseAssert(false && "Out Of Memory");
            }
            assert(addr != nullptr);
            result = reinterpret_cast<WasmBranchManager*>(addr);
        }

        memset(result, -1, sizeof(WasmBranchManager));

        uint8_t* buf = reinterpret_cast<uint8_t*>(result + 1);
        for (uint32_t i = 0; i <= x_maxIntRegs; i++)
        {
            WasmBranchOpcodeStencil* s = PrepareCondBrWithoutOutput(buf, i);
            uint64_t diff = reinterpret_cast<uint64_t>(s) - reinterpret_cast<uint64_t>(result);
            ReleaseAssert(diff < std::numeric_limits<OffsetType>::max());
            result->m_condBrWithoutOutput[i] = static_cast<OffsetType>(diff);
        }

        for (bool isU32 : {false, true})
        {
            for (uint32_t oldNumInt = 0; oldNumInt <= x_maxIntRegs; oldNumInt++)
            {
                for (uint32_t newNumInt = 0; newNumInt <= x_maxIntRegs; newNumInt++)
                {
                    for (bool spillOutput : {false, true})
                    {
                        WasmBranchOpcodeStencil* s = PrepareCondBrWithIntOutput(buf, isU32, oldNumInt, newNumInt, spillOutput);
                        if (s == nullptr) { continue; }
                        uint64_t diff = reinterpret_cast<uint64_t>(s) - reinterpret_cast<uint64_t>(result);
                        ReleaseAssert(diff < std::numeric_limits<OffsetType>::max());
                        result->m_condBrWithIntOutput[isU32][oldNumInt][newNumInt][spillOutput] = static_cast<OffsetType>(diff);
                    }
                }
            }
        }

        for (bool isFloat : {false, true})
        {
            for (uint32_t oldNumInt = 0; oldNumInt <= x_maxIntRegs; oldNumInt++)
            {
                for (uint32_t oldNumFloat = 0; oldNumFloat <= x_maxFloatRegs; oldNumFloat++)
                {
                    for (uint32_t newNumFloat = 0; newNumFloat <= x_maxFloatRegs; newNumFloat++)
                    {
                        for (bool spillOutput : {false, true})
                        {
                            WasmBranchOpcodeStencil* s = PrepareCondBrWithFloatOutput(buf, isFloat, oldNumInt, oldNumFloat, newNumFloat, spillOutput);
                            if (s == nullptr) { continue; }
                            uint64_t diff = reinterpret_cast<uint64_t>(s) - reinterpret_cast<uint64_t>(result);
                            ReleaseAssert(diff < std::numeric_limits<OffsetType>::max());
                            result->m_condBrWithFloatOutput[isFloat][oldNumInt][oldNumFloat][newNumFloat][spillOutput] = static_cast<OffsetType>(diff);
                        }
                    }
                }
            }
        }

        for (bool isU32 : {false, true})
        {
            for (uint32_t oldNumInt = 0; oldNumInt <= x_maxIntRegs; oldNumInt++)
            {
                for (uint32_t newNumInt = 0; newNumInt <= x_maxIntRegs; newNumInt++)
                {
                    for (bool spillOutput : {false, true})
                    {
                        WasmBranchOpcodeStencil* s = PrepareBranchWithIntOutput(buf, isU32, oldNumInt, newNumInt, spillOutput);
                        if (s == nullptr) { continue; }
                        uint64_t diff = reinterpret_cast<uint64_t>(s) - reinterpret_cast<uint64_t>(result);
                        ReleaseAssert(diff < std::numeric_limits<OffsetType>::max());
                        result->m_brWithIntOutput[isU32][oldNumInt][newNumInt][spillOutput] = static_cast<OffsetType>(diff);
                    }
                }
            }
        }

        for (bool isFloat : {false, true})
        {
            for (uint32_t oldNumFloat = 0; oldNumFloat <= x_maxFloatRegs; oldNumFloat++)
            {
                for (uint32_t newNumFloat = 0; newNumFloat <= x_maxFloatRegs; newNumFloat++)
                {
                    for (bool spillOutput : {false, true})
                    {
                        WasmBranchOpcodeStencil* s = PrepareBranchWithFloatOutput(buf, isFloat, oldNumFloat, newNumFloat, spillOutput);
                        if (s == nullptr) { continue; }
                        uint64_t diff = reinterpret_cast<uint64_t>(s) - reinterpret_cast<uint64_t>(result);
                        ReleaseAssert(diff < std::numeric_limits<OffsetType>::max());
                        result->m_brWithFloatOutput[isFloat][oldNumFloat][newNumFloat][spillOutput] = static_cast<OffsetType>(diff);
                    }
                }
            }
        }

        for (uint32_t numInt = 0; numInt <= x_maxIntRegs; numInt++)
        {
            WasmBranchOpcodeStencil* s = PrepareIfBranch(buf, numInt);
            uint64_t diff = reinterpret_cast<uint64_t>(s) - reinterpret_cast<uint64_t>(result);
            ReleaseAssert(diff < std::numeric_limits<OffsetType>::max());
            result->m_ifBranch[numInt] = static_cast<OffsetType>(diff);
        }

        ReleaseAssert(reinterpret_cast<uint64_t>(buf) - reinterpret_cast<uint64_t>(result) <= len + sizeof(WasmBranchManager));
        return result;
    }

    static WasmBranchOpcodeStencil* PrepareCondBrWithoutOutput(uint8_t*& buf, uint32_t numInt)
    {
        std::vector<uint8_t> jumpContents;
        std::vector<std::pair<uint8_t, uint8_t>> fixups;
        ProcessConditionalJumpStencil(FastInterpBoilerplateLibrary<FICondBranchImpl>::SelectBoilerplateBluePrint(
                                          static_cast<FINumOpaqueIntegralParams>(numInt > 0 ? numInt - 1 : 0),
                                          FIOpaqueParamsHelper::GetMaxOFP(),
                                          numInt > 0 /*isInRegister*/),
                                      true /*shortenJump*/,
                                      jumpContents /*out*/,
                                      fixups /*out*/);
        jumpContents.push_back(x86_64_rip_relative_jmp_instruction_len);
        jumpContents.push_back(x86_64_jmp_instruction_opcode);

        WasmBranchOpcodeStencil* s = reinterpret_cast<WasmBranchOpcodeStencil*>(buf);
        buf += sizeof(WasmBranchOpcodeStencil);
        ReleaseAssert(jumpContents.size() < 124);
        s->m_contentLenBytes = static_cast<uint8_t>(jumpContents.size() + 4);
        s->m_targetSlotOffset =static_cast<uint8_t>(jumpContents.size());
        s->m_sym32FixupArrayLenBytes = static_cast<uint8_t>(fixups.size()) * 2;
        memcpy(buf, jumpContents.data(), jumpContents.size());
        UnalignedWrite<uint32_t>(buf + jumpContents.size(), -static_cast<uint32_t>(jumpContents.size() + 4));

        buf += jumpContents.size() + 4;
        for (uint64_t i = 0; i < fixups.size(); i++)
        {
            *buf = fixups[i].first;
            buf++;
            *buf = fixups[i].second;
            buf++;
        }
        return s;
    }

    static WasmBranchOpcodeStencil* PrepareCondBrWithIntOutput(uint8_t*& buf, bool isU32, uint32_t oldNumInt, uint32_t newNumInt, bool spillOutput)
    {
        if (spillOutput && newNumInt > 0) { return nullptr; }
        if (newNumInt == x_maxIntRegs && !spillOutput) { return nullptr; }
        std::vector<uint8_t> jumpContents;
        std::vector<std::pair<uint8_t, uint8_t>> fixups;
        ProcessConditionalJumpStencil(FastInterpBoilerplateLibrary<FICondBranchImpl>::SelectBoilerplateBluePrint(
                                          static_cast<FINumOpaqueIntegralParams>(oldNumInt > 0 ? oldNumInt - 1 : 0),
                                          FIOpaqueParamsHelper::GetMaxOFP(),
                                          oldNumInt > 0 /*isInRegister*/),
                                      true /*shortenJump*/,
                                      jumpContents /*out*/,
                                      fixups /*out*/);
        bool isInStack2ndTop = (oldNumInt == 0);
        if (oldNumInt > 0) { oldNumInt--; }

        bool isInRegister = (oldNumInt > 0);
        if (oldNumInt > 0) { oldNumInt--; }
        if (newNumInt > oldNumInt) { return nullptr; }
        std::vector<uint8_t> storeContents;
        std::vector<std::pair<uint8_t, uint8_t>> storeFixups;
        ProcessStoreResultStencil(FastInterpBoilerplateLibrary<FIStoreBlockSimpleResultImpl>::SelectBoilerplateBluePrint(
                                      isU32 ? FastInterpTypeId::Get<uint32_t>() : FastInterpTypeId::Get<uint64_t>(),
                                      static_cast<FINumOpaqueIntegralParams>(oldNumInt),
                                      FIOpaqueParamsHelper::GetMaxOFP(),
                                      static_cast<NumIntegralParamsAfterBlock>(newNumInt),
                                      static_cast<NumFloatParamsAfterBlock>(x_maxFloatRegs),
                                      isInRegister,
                                      isInStack2ndTop,
                                      spillOutput),
                                  storeContents /*out*/,
                                  storeFixups /*out*/);
        ReleaseAssert(storeContents.size() < 120);
        jumpContents.push_back(static_cast<uint8_t>(storeContents.size() + x86_64_rip_relative_jmp_instruction_len));

        WasmBranchOpcodeStencil* s = reinterpret_cast<WasmBranchOpcodeStencil*>(buf);
        buf += sizeof(WasmBranchOpcodeStencil);
        s->m_contentLenBytes = static_cast<uint8_t>(jumpContents.size() + storeContents.size() + x86_64_rip_relative_jmp_instruction_len);
        s->m_targetSlotOffset =static_cast<uint8_t>(jumpContents.size() + storeContents.size() + 1);
        s->m_sym32FixupArrayLenBytes = static_cast<uint8_t>(fixups.size() + storeFixups.size()) * 2;
        memcpy(buf, jumpContents.data(), jumpContents.size());
        memcpy(buf + jumpContents.size(), storeContents.data(), storeContents.size());
        buf[jumpContents.size() + storeContents.size()] = x86_64_jmp_instruction_opcode;
        UnalignedWrite<uint32_t>(buf + jumpContents.size() + storeContents.size() + 1, -static_cast<uint32_t>(s->m_contentLenBytes));

        buf += s->m_contentLenBytes;

        for (std::pair<uint8_t, uint8_t> c : storeFixups)
        {
            fixups.push_back(std::make_pair(c.first, static_cast<uint8_t>(c.second + jumpContents.size())));
        }
        std::sort(fixups.begin(), fixups.end());

        for (uint64_t i = 0; i < fixups.size(); i++)
        {
            *buf = fixups[i].first;
            buf++;
            *buf = fixups[i].second;
            buf++;
        }
        return s;
    }

    static WasmBranchOpcodeStencil* PrepareCondBrWithFloatOutput(uint8_t*& buf, bool isFloat, uint32_t oldNumInt, uint32_t oldNumFloat, uint32_t newNumFloat, bool spillOutput)
    {
        if (spillOutput && newNumFloat > 0) { return nullptr; }
        if (newNumFloat == x_maxFloatRegs && !spillOutput) { return nullptr; }

        std::vector<uint8_t> jumpContents;
        std::vector<std::pair<uint8_t, uint8_t>> fixups;
        ProcessConditionalJumpStencil(FastInterpBoilerplateLibrary<FICondBranchImpl>::SelectBoilerplateBluePrint(
                                          static_cast<FINumOpaqueIntegralParams>(oldNumInt > 0 ? oldNumInt - 1 : 0),
                                          FIOpaqueParamsHelper::GetMaxOFP(),
                                          oldNumInt > 0 /*isInRegister*/),
                                      true /*shortenJump*/,
                                      jumpContents /*out*/,
                                      fixups /*out*/);

        bool isInRegister = (oldNumFloat > 0);
        if (oldNumFloat > 0) { oldNumFloat--; }
        if (newNumFloat > oldNumFloat) { return nullptr; }

        std::vector<uint8_t> storeContents;
        std::vector<std::pair<uint8_t, uint8_t>> storeFixups;
        ProcessStoreResultStencil(FastInterpBoilerplateLibrary<FIStoreBlockSimpleResultImpl>::SelectBoilerplateBluePrint(
                                      isFloat ? FastInterpTypeId::Get<float>() : FastInterpTypeId::Get<double>(),
                                      FIOpaqueParamsHelper::GetMaxOIP(),
                                      static_cast<FINumOpaqueFloatingParams>(oldNumFloat),
                                      static_cast<NumIntegralParamsAfterBlock>(x_maxIntRegs),
                                      static_cast<NumFloatParamsAfterBlock>(newNumFloat),
                                      isInRegister,
                                      false /*isStack2ndTop*/,
                                      spillOutput),
                                  storeContents /*out*/,
                                  storeFixups /*out*/);
        ReleaseAssert(storeContents.size() < 120);
        jumpContents.push_back(static_cast<uint8_t>(storeContents.size() + x86_64_rip_relative_jmp_instruction_len));

        WasmBranchOpcodeStencil* s = reinterpret_cast<WasmBranchOpcodeStencil*>(buf);
        buf += sizeof(WasmBranchOpcodeStencil);
        s->m_contentLenBytes = static_cast<uint8_t>(jumpContents.size() + storeContents.size() + x86_64_rip_relative_jmp_instruction_len);
        s->m_targetSlotOffset =static_cast<uint8_t>(jumpContents.size() + storeContents.size() + 1);
        s->m_sym32FixupArrayLenBytes = static_cast<uint8_t>(fixups.size() + storeFixups.size()) * 2;
        memcpy(buf, jumpContents.data(), jumpContents.size());
        memcpy(buf + jumpContents.size(), storeContents.data(), storeContents.size());
        buf[jumpContents.size() + storeContents.size()] = x86_64_jmp_instruction_opcode;
        UnalignedWrite<uint32_t>(buf + jumpContents.size() + storeContents.size() + 1, -static_cast<uint32_t>(s->m_contentLenBytes));

        buf += s->m_contentLenBytes;

        for (std::pair<uint8_t, uint8_t> c : storeFixups)
        {
            fixups.push_back(std::make_pair(c.first, static_cast<uint8_t>(c.second + jumpContents.size())));
        }
        std::sort(fixups.begin(), fixups.end());

        for (uint64_t i = 0; i < fixups.size(); i++)
        {
            *buf = fixups[i].first;
            buf++;
            *buf = fixups[i].second;
            buf++;
        }
        return s;
    }

    static WasmBranchOpcodeStencil* PrepareBranchWithIntOutput(uint8_t*& buf, bool isU32, uint32_t oldNumInt, uint32_t newNumInt, bool spillOutput)
    {
        if (spillOutput && newNumInt > 0) { return nullptr; }
        if (newNumInt == x_maxIntRegs && !spillOutput) { return nullptr; }
        bool isInRegister = (oldNumInt > 0);
        if (oldNumInt > 0) { oldNumInt--; }

        if (newNumInt > oldNumInt) { return nullptr; }

        std::vector<uint8_t> storeContents;
        std::vector<std::pair<uint8_t, uint8_t>> storeFixups;
        ProcessStoreResultStencil(FastInterpBoilerplateLibrary<FIStoreBlockSimpleResultImpl>::SelectBoilerplateBluePrint(
                                      isU32 ? FastInterpTypeId::Get<uint32_t>() : FastInterpTypeId::Get<uint64_t>(),
                                      static_cast<FINumOpaqueIntegralParams>(oldNumInt),
                                      FIOpaqueParamsHelper::GetMaxOFP(),
                                      static_cast<NumIntegralParamsAfterBlock>(newNumInt),
                                      static_cast<NumFloatParamsAfterBlock>(x_maxFloatRegs),
                                      isInRegister,
                                      false /*isInStack2ndTop*/,
                                      spillOutput),
                                  storeContents /*out*/,
                                  storeFixups /*out*/);
        storeContents.push_back(x86_64_jmp_instruction_opcode);

        WasmBranchOpcodeStencil* s = reinterpret_cast<WasmBranchOpcodeStencil*>(buf);
        buf += sizeof(WasmBranchOpcodeStencil);
        s->m_contentLenBytes = static_cast<uint8_t>(storeContents.size() + 4);
        s->m_targetSlotOffset =static_cast<uint8_t>(storeContents.size());
        s->m_sym32FixupArrayLenBytes = static_cast<uint8_t>(storeFixups.size()) * 2;
        memcpy(buf, storeContents.data(), storeContents.size());
        UnalignedWrite<uint32_t>(buf + storeContents.size(), -static_cast<uint32_t>(s->m_contentLenBytes));

        buf += s->m_contentLenBytes;

        for (uint64_t i = 0; i < storeFixups.size(); i++)
        {
            *buf = storeFixups[i].first;
            buf++;
            *buf = storeFixups[i].second;
            buf++;
        }
        return s;
    }

    static WasmBranchOpcodeStencil* PrepareBranchWithFloatOutput(uint8_t*& buf, bool isFloat, uint32_t oldNumFloat, uint32_t newNumFloat, bool spillOutput)
    {
        if (spillOutput && newNumFloat > 0) { return nullptr; }
        if (newNumFloat == x_maxIntRegs && !spillOutput) { return nullptr; }
        bool isInRegister = (oldNumFloat > 0);
        if (oldNumFloat > 0) { oldNumFloat--; }

        if (newNumFloat > oldNumFloat) { return nullptr; }

        std::vector<uint8_t> storeContents;
        std::vector<std::pair<uint8_t, uint8_t>> storeFixups;
        ProcessStoreResultStencil(FastInterpBoilerplateLibrary<FIStoreBlockSimpleResultImpl>::SelectBoilerplateBluePrint(
                                      isFloat ? FastInterpTypeId::Get<float>() : FastInterpTypeId::Get<double>(),
                                      FIOpaqueParamsHelper::GetMaxOIP(),
                                      static_cast<FINumOpaqueFloatingParams>(oldNumFloat),
                                      static_cast<NumIntegralParamsAfterBlock>(x_maxIntRegs),
                                      static_cast<NumFloatParamsAfterBlock>(newNumFloat),
                                      isInRegister,
                                      false /*isInStack2ndTop*/,
                                      spillOutput),
                                  storeContents /*out*/,
                                  storeFixups /*out*/);
        storeContents.push_back(x86_64_jmp_instruction_opcode);

        WasmBranchOpcodeStencil* s = reinterpret_cast<WasmBranchOpcodeStencil*>(buf);
        buf += sizeof(WasmBranchOpcodeStencil);
        s->m_contentLenBytes = static_cast<uint8_t>(storeContents.size() + 4);
        s->m_targetSlotOffset =static_cast<uint8_t>(storeContents.size());
        s->m_sym32FixupArrayLenBytes = static_cast<uint8_t>(storeFixups.size()) * 2;
        memcpy(buf, storeContents.data(), storeContents.size());
        UnalignedWrite<uint32_t>(buf + storeContents.size(), -static_cast<uint32_t>(s->m_contentLenBytes));

        buf += s->m_contentLenBytes;

        for (uint64_t i = 0; i < storeFixups.size(); i++)
        {
            *buf = storeFixups[i].first;
            buf++;
            *buf = storeFixups[i].second;
            buf++;
        }
        return s;
    }

    static WasmBranchOpcodeStencil* PrepareIfBranch(uint8_t*& buf, uint32_t numInt)
    {
        std::vector<uint8_t> jumpContents;
        std::vector<std::pair<uint8_t, uint8_t>> fixups;
        ProcessConditionalJumpStencil(FastInterpBoilerplateLibrary<FICondBranchImpl>::SelectBoilerplateBluePrint(
                                          static_cast<FINumOpaqueIntegralParams>(numInt > 0 ? numInt - 1 : 0),
                                          FIOpaqueParamsHelper::GetMaxOFP(),
                                          numInt > 0 /*isInRegister*/),
                                      false /*shortenJump*/,
                                      jumpContents /*out*/,
                                      fixups /*out*/);

        WasmBranchOpcodeStencil* s = reinterpret_cast<WasmBranchOpcodeStencil*>(buf);
        buf += sizeof(WasmBranchOpcodeStencil);
        s->m_contentLenBytes = static_cast<uint8_t>(jumpContents.size() + 4);
        s->m_targetSlotOffset =static_cast<uint8_t>(jumpContents.size());
        s->m_sym32FixupArrayLenBytes = static_cast<uint8_t>(fixups.size()) * 2;
        memcpy(buf, jumpContents.data(), jumpContents.size());
        UnalignedWrite<uint32_t>(buf + jumpContents.size(), -static_cast<uint32_t>(s->m_contentLenBytes));

        buf += s->m_contentLenBytes;

        for (uint64_t i = 0; i < fixups.size(); i++)
        {
            *buf = fixups[i].first;
            buf++;
            *buf = fixups[i].second;
            buf++;
        }
        return s;
    }

    static void ProcessStoreResultStencil(const FastInterpBoilerplateBluePrint* blueprint,
                                          std::vector<uint8_t>& contentOutput,
                                          std::vector<std::pair<uint8_t, uint8_t>>& fixupOutput)
    {
        ReleaseAssert(blueprint->m_addr32FixupArrayLength == 1 &&
                      blueprint->m_addr32FixupArray[0] == blueprint->m_contentLength - 4);
        ReleaseAssert(blueprint->m_symbol32FixupArrayLength > 0 &&
                      blueprint->m_symbol32FixupArray[blueprint->m_symbol32FixupArrayLength - 1].m_offset == blueprint->m_contentLength - 4);
        ReleaseAssert(blueprint->m_symbol64FixupArrayLength == 0);
        ReleaseAssert(blueprint->m_highestCppFnptrPlaceholderOrdinal == 0);
        ReleaseAssert(blueprint->m_highestBoilerplateFnptrPlaceholderOrdinal == 1);

        contentOutput.clear();
        contentOutput.resize(blueprint->m_contentLength - 5);
        memcpy(contentOutput.data(), blueprint->m_content, blueprint->m_contentLength - 5);

        fixupOutput.clear();
        for (uint32_t i = 0; i < blueprint->m_symbol32FixupArrayLength - 1; i++)
        {
            FastInterpSymbolFixupRecord record = blueprint->m_symbol32FixupArray[i];
            if (record.m_ordinalIntoPlaceholderArray == 0)
            {
                ReleaseAssert(record.m_offset == blueprint->m_contentLength - 4);
                continue;
            }
            ReleaseAssert(record.m_offset + sizeof(uint32_t) <= blueprint->m_contentLength);
            uint32_t dataOrd = record.m_ordinalIntoPlaceholderArray - 1;
            ReleaseAssert(dataOrd <= 2 || (8 <= dataOrd && dataOrd <= 12));
            if (8 <= dataOrd && dataOrd <= 12)
            {
                if (dataOrd == 8)
                {
                    dataOrd = 0;
                    UnalignedAddAndWriteback<uint32_t>(contentOutput.data() + record.m_offset, 8);
                }
                else if (dataOrd == 9)
                {
                    dataOrd = 0;
                    UnalignedAddAndWriteback<uint32_t>(contentOutput.data() + record.m_offset, static_cast<uint32_t>(-8));
                }
                else if (dataOrd == 10)
                {
                    dataOrd = 0;
                    UnalignedAddAndWriteback<uint32_t>(contentOutput.data() + record.m_offset, static_cast<uint32_t>(-16));
                }
                else if (dataOrd == 11)
                {
                    dataOrd = 1;
                    UnalignedAddAndWriteback<uint32_t>(contentOutput.data() + record.m_offset, 8);
                }
                else
                {
                    ReleaseAssert(dataOrd == 12);
                    dataOrd = 1;
                    UnalignedAddAndWriteback<uint32_t>(contentOutput.data() + record.m_offset, static_cast<uint32_t>(-8));
                }
            }
            ReleaseAssert(0 <= dataOrd && dataOrd <= 2);
            ReleaseAssert(record.m_offset <= 255);
            fixupOutput.push_back(std::make_pair(static_cast<uint8_t>(dataOrd), static_cast<uint8_t>(record.m_offset)));
        }
        std::sort(fixupOutput.begin(), fixupOutput.end());
    }

    static void ProcessConditionalJumpStencil(const FastInterpBoilerplateBluePrint* blueprint,
                                              bool shortenJump,
                                              std::vector<uint8_t>& contentOutput,
                                              std::vector<std::pair<uint8_t, uint8_t>>& fixupOutput)
    {
        ReleaseAssert(blueprint->m_contentLength >= 11);
        ReleaseAssert(blueprint->m_addr32FixupArrayLength == 2 &&
                      blueprint->m_addr32FixupArray[0] == blueprint->m_contentLength - 9 &&
                      blueprint->m_addr32FixupArray[1] == blueprint->m_contentLength - 4);
        ReleaseAssert(blueprint->m_content[blueprint->m_contentLength - 5] == x86_64_jmp_instruction_opcode);
        ReleaseAssert(blueprint->m_content[blueprint->m_contentLength - 11] == 0x0F);
        ReleaseAssert(0x80 <= blueprint->m_content[blueprint->m_contentLength - 10] && blueprint->m_content[blueprint->m_contentLength - 10] <= 0x8F);
        ReleaseAssert(blueprint->m_symbol32FixupArrayLength > 0 &&
                      blueprint->m_symbol32FixupArray[blueprint->m_symbol32FixupArrayLength - 1].m_offset == blueprint->m_contentLength - 4);
        ReleaseAssert(blueprint->m_symbol64FixupArrayLength == 0);
        ReleaseAssert(blueprint->m_highestCppFnptrPlaceholderOrdinal == 0);
        ReleaseAssert(blueprint->m_highestBoilerplateFnptrPlaceholderOrdinal == 2);

        contentOutput.clear();
        if (!shortenJump)
        {
            contentOutput.resize(blueprint->m_contentLength - 9);
            memcpy(contentOutput.data(), blueprint->m_content, blueprint->m_contentLength - 9);
        }
        else
        {
            contentOutput.resize(blueprint->m_contentLength - 10);
            memcpy(contentOutput.data(), blueprint->m_content, blueprint->m_contentLength - 11);
            contentOutput[blueprint->m_contentLength - 11] = blueprint->m_content[blueprint->m_contentLength - 10] - 0x10;
        }

        fixupOutput.clear();
        for (uint32_t i = 0; i < blueprint->m_symbol32FixupArrayLength - 1; i++)
        {
            FastInterpSymbolFixupRecord record = blueprint->m_symbol32FixupArray[i];
            if (record.m_ordinalIntoPlaceholderArray == 0)
            {
                ReleaseAssert(record.m_offset == blueprint->m_contentLength - 4);
                continue;
            }
            if (record.m_ordinalIntoPlaceholderArray == 1)
            {
                ReleaseAssert(record.m_offset == blueprint->m_contentLength - 9);
                continue;
            }
            ReleaseAssert(record.m_offset + sizeof(uint32_t) <= blueprint->m_contentLength);
            uint32_t dataOrd = record.m_ordinalIntoPlaceholderArray - 2;
            ReleaseAssert(dataOrd <= 2 || (8 <= dataOrd && dataOrd <= 12));
            if (8 <= dataOrd && dataOrd <= 12)
            {
                if (dataOrd == 8)
                {
                    dataOrd = 0;
                    UnalignedAddAndWriteback<uint32_t>(contentOutput.data() + record.m_offset, 8);
                }
                else if (dataOrd == 9)
                {
                    dataOrd = 0;
                    UnalignedAddAndWriteback<uint32_t>(contentOutput.data() + record.m_offset, static_cast<uint32_t>(-8));
                }
                else if (dataOrd == 10)
                {
                    dataOrd = 0;
                    UnalignedAddAndWriteback<uint32_t>(contentOutput.data() + record.m_offset, static_cast<uint32_t>(-16));
                }
                else if (dataOrd == 11)
                {
                    dataOrd = 1;
                    UnalignedAddAndWriteback<uint32_t>(contentOutput.data() + record.m_offset, 8);
                }
                else
                {
                    ReleaseAssert(dataOrd == 12);
                    dataOrd = 1;
                    UnalignedAddAndWriteback<uint32_t>(contentOutput.data() + record.m_offset, static_cast<uint32_t>(-8));
                }
            }
            ReleaseAssert(0 <= dataOrd && dataOrd <= 2);
            ReleaseAssert(record.m_offset <= 255);
            fixupOutput.push_back(std::make_pair(static_cast<uint8_t>(dataOrd), static_cast<uint8_t>(record.m_offset)));
        }
        std::sort(fixupOutput.begin(), fixupOutput.end());
    }

    static constexpr int x_maxIntRegs = WasmCommonOpcodeManager::x_maxIntRegs;
    static constexpr int x_maxFloatRegs = WasmCommonOpcodeManager::x_maxFloatRegs;

    OffsetType m_condBrWithIntOutput[2][x_maxIntRegs + 1][x_maxIntRegs + 1][2];
    OffsetType m_condBrWithFloatOutput[2][x_maxIntRegs + 1][x_maxFloatRegs + 1][x_maxFloatRegs + 1][2];
    OffsetType m_condBrWithoutOutput[x_maxIntRegs + 1];
    OffsetType m_brWithIntOutput[2][x_maxIntRegs + 1][x_maxIntRegs + 1][2];
    OffsetType m_brWithFloatOutput[2][x_maxFloatRegs + 1][x_maxFloatRegs + 1][2];
    OffsetType m_ifBranch[x_maxIntRegs + 1];
};

extern WasmBranchManager* g_wasmBranchManager;
WasmBranchManager* g_wasmBranchManager = WasmBranchManager::Build();

class WasmCppEntryManager
{
public:
    WasmCppEntryManager()
    {
        Populate(m_desc[0], FastInterpBoilerplateLibrary<FICdeclInterfaceImpl>::SelectBoilerplateBluePrint(FastInterpTypeId::Get<uint32_t>(), true));
        Populate(m_desc[1], FastInterpBoilerplateLibrary<FICdeclInterfaceImpl>::SelectBoilerplateBluePrint(FastInterpTypeId::Get<uint64_t>(), true));
        Populate(m_desc[2], FastInterpBoilerplateLibrary<FICdeclInterfaceImpl>::SelectBoilerplateBluePrint(FastInterpTypeId::Get<float>(), true));
        Populate(m_desc[3], FastInterpBoilerplateLibrary<FICdeclInterfaceImpl>::SelectBoilerplateBluePrint(FastInterpTypeId::Get<double>(), true));
        Populate(m_desc[4], FastInterpBoilerplateLibrary<FICdeclInterfaceImpl>::SelectBoilerplateBluePrint(FastInterpTypeId::Get<void>(), true));
    }

    void Codegen(uint8_t*& dstAddr, WasmValueType returnType, uint8_t* funcAddr)
    {
        Desc& d = m_desc[static_cast<uint32_t>(returnType)];
        memcpy(dstAddr, d.m_addr, d.m_len);
        UnalignedAddAndWriteback<uint32_t>(dstAddr + d.m_patchOffset, static_cast<uint32_t>(funcAddr - dstAddr));
        dstAddr += d.m_len;
    }

private:
    struct Desc
    {
        const uint8_t* m_addr;
        uint32_t m_len;
        uint32_t m_patchOffset;
    };

    void Populate(Desc& out, const FastInterpBoilerplateBluePrint* blueprint)
    {
        ReleaseAssert(blueprint->m_addr32FixupArrayLength == 1);
        ReleaseAssert(blueprint->m_symbol32FixupArrayLength == 1);
        ReleaseAssert(blueprint->m_symbol64FixupArrayLength == 0);
        ReleaseAssert(blueprint->m_highestUInt64PlaceholderOrdinal == 0);
        ReleaseAssert(blueprint->m_highestBoilerplateFnptrPlaceholderOrdinal == 1);
        ReleaseAssert(blueprint->m_highestCppFnptrPlaceholderOrdinal == 0);
        ReleaseAssert(blueprint->m_addr32FixupArray[0] == blueprint->m_symbol32FixupArray[0].m_offset);
        ReleaseAssert(blueprint->m_symbol32FixupArray[0].m_ordinalIntoPlaceholderArray == 0);
        out.m_addr = blueprint->m_content;
        out.m_len = blueprint->m_contentLength;
        out.m_patchOffset = blueprint->m_addr32FixupArray[0];
    }

    Desc m_desc[5];
};

extern WasmCppEntryManager g_wasmCppEntryManager;
WasmCppEntryManager g_wasmCppEntryManager;

class WasmCallManager
{
public:
    struct Desc
    {
        const uint8_t* m_addr;
        uint16_t m_len;
        uint16_t m_patchOffset;
        uint16_t m_patchOffset2;
        uint16_t m_patchOrd;
    };

    WasmCallManager()
    {
        for (bool spillOutput : { false, true })
        {
            Process(m_part1[0][spillOutput], m_part2[0][spillOutput], FastInterpBoilerplateLibrary<FICallExprImpl>::SelectBoilerplateBluePrint(
                        FastInterpTypeId::Get<uint32_t>(),
                        FIOpaqueParamsHelper::GetMaxOIP(),
                        FIOpaqueParamsHelper::GetMaxOFP(),
                        spillOutput));
            Process(m_part1[1][spillOutput], m_part2[1][spillOutput], FastInterpBoilerplateLibrary<FICallExprImpl>::SelectBoilerplateBluePrint(
                        FastInterpTypeId::Get<uint64_t>(),
                        FIOpaqueParamsHelper::GetMaxOIP(),
                        FIOpaqueParamsHelper::GetMaxOFP(),
                        spillOutput));
            Process(m_part1[2][spillOutput], m_part2[2][spillOutput], FastInterpBoilerplateLibrary<FICallExprImpl>::SelectBoilerplateBluePrint(
                        FastInterpTypeId::Get<float>(),
                        FIOpaqueParamsHelper::GetMaxOIP(),
                        FIOpaqueParamsHelper::GetMaxOFP(),
                        spillOutput));
            Process(m_part1[3][spillOutput], m_part2[3][spillOutput], FastInterpBoilerplateLibrary<FICallExprImpl>::SelectBoilerplateBluePrint(
                        FastInterpTypeId::Get<double>(),
                        FIOpaqueParamsHelper::GetMaxOIP(),
                        FIOpaqueParamsHelper::GetMaxOFP(),
                        spillOutput));
            Process(m_part1[4][spillOutput], m_part2[4][spillOutput], FastInterpBoilerplateLibrary<FICallExprImpl>::SelectBoilerplateBluePrint(
                        FastInterpTypeId::Get<void>(),
                        FIOpaqueParamsHelper::GetMaxOIP(),
                        FIOpaqueParamsHelper::GetMaxOFP(),
                        false));
        }
    }

    uint8_t* WARN_UNUSED EmitPrepare(uint8_t*& dstAddr, WasmValueType returnType, bool spillReturnValue)
    {
        Desc& d = m_part1[static_cast<uint8_t>(returnType)][spillReturnValue];
        memcpy(dstAddr, d.m_addr, d.m_len);
        uint8_t* r = dstAddr + d.m_patchOffset;
        dstAddr += d.m_len;
        return r;
    }

    uint8_t* WARN_UNUSED EmitCall(uint8_t*& dstAddr)
    {
        *dstAddr = 0xe8;
        UnalignedWrite<uint32_t>(dstAddr + 1, static_cast<uint32_t>(-reinterpret_cast<uint64_t>(dstAddr + 5)));
        dstAddr += 5;
        return dstAddr - 4;
    }

    uint8_t* WARN_UNUSED EmitCleanup(uint8_t*& dstAddr, WasmValueType returnType, bool spillReturnValue, WasmCommonOpcodeFixups* fixup)
    {
        Desc& d = m_part2[static_cast<uint8_t>(returnType)][spillReturnValue];
        memcpy(dstAddr, d.m_addr, d.m_len);
        if (spillReturnValue)
        {
            assert(0 <= d.m_patchOrd && d.m_patchOrd <= 1);
            UnalignedWrite<uint32_t>(dstAddr + d.m_patchOffset2, static_cast<uint32_t>(fixup->m_data[d.m_patchOrd] + 8));
        }
        else
        {
            assert(d.m_patchOrd == static_cast<uint16_t>(-1));
        }
        uint8_t* r = dstAddr + d.m_patchOffset;
        dstAddr += d.m_len;
        return r;
    }

private:
    void Process(Desc& out1, Desc& out2, const FastInterpBoilerplateBluePrint* blueprint)
    {
        ReleaseAssert(blueprint->m_addr32FixupArrayLength == 2);
        ReleaseAssert(blueprint->m_symbol32FixupArrayLength == 2 || blueprint->m_symbol32FixupArrayLength == 3);
        ReleaseAssert(blueprint->m_symbol64FixupArrayLength == 0);
        ReleaseAssert(blueprint->m_highestBoilerplateFnptrPlaceholderOrdinal == 2);
        ReleaseAssert(blueprint->m_highestCppFnptrPlaceholderOrdinal == 0);
        ReleaseAssert(UnalignedRead<uint32_t>(blueprint->m_content + 3) == 0x218);
        ReleaseAssert(UnalignedRead<uint32_t>(blueprint->m_content + blueprint->m_contentLength - 9) == 0x218);
        out1.m_addr = blueprint->m_content;
        out1.m_len = static_cast<uint16_t>(blueprint->m_addr32FixupArray[0] - 1);
        out1.m_patchOffset = 3;
        out2.m_addr = blueprint->m_content + blueprint->m_addr32FixupArray[0] + 4;
        out2.m_len = static_cast<uint16_t>(blueprint->m_contentLength - out1.m_len - 10);
        out2.m_patchOffset = static_cast<uint16_t>(out2.m_len - 4);
        ReleaseAssert(UnalignedRead<uint32_t>(out2.m_addr + out2.m_patchOffset) == 0x218);
        if (blueprint->m_symbol32FixupArrayLength == 3)
        {
            ReleaseAssert(blueprint->m_symbol32FixupArray[1].m_ordinalIntoPlaceholderArray == 10 ||
                          blueprint->m_symbol32FixupArray[1].m_ordinalIntoPlaceholderArray == 13);
            out2.m_patchOffset2 = static_cast<uint16_t>(blueprint->m_symbol32FixupArray[1].m_offset - (out2.m_addr - blueprint->m_content));
            out2.m_patchOrd = blueprint->m_symbol32FixupArray[1].m_ordinalIntoPlaceholderArray == 10 ? 0 : 1;
        }
        else
        {
            out2.m_patchOrd = static_cast<uint16_t>(-1);
        }
    }

    Desc m_part1[5][2];
    Desc m_part2[5][2];
};

extern WasmCallManager g_wasmCallManager;
WasmCallManager g_wasmCallManager;

class WasmBrTableManager
{
public:
    WasmBrTableManager()
    {
        for (uint32_t i = 0; i <= WasmCommonOpcodeManager::x_maxIntRegs; i++)
        {
            Process(m_desc[i], FastInterpBoilerplateLibrary<FIBrTableImpl>::SelectBoilerplateBluePrint(
                        static_cast<FINumOpaqueIntegralParams>((i > 0) ? i - 1 : 0),
                        FIOpaqueParamsHelper::GetMaxOFP(),
                        i > 0));
        }
    }

    void Codegen(uint8_t*& dstAddr, uint32_t numInRegisterInt, WasmCommonOpcodeFixups* fixups)
    {
        Desc& d = m_desc[numInRegisterInt];
        memcpy(dstAddr, d.m_addr, d.m_len);
        for (uint16_t i = 0; i < d.m_numPatches; i++)
        {
            UnalignedWrite<uint32_t>(dstAddr + d.m_patches[i][0], static_cast<uint32_t>(fixups->m_data[d.m_patches[i][1]]));
        }
        UnalignedWrite<uint16_t>(dstAddr + d.m_len, 0xe0ff);
        dstAddr += d.m_len + 2;
    }

private:
    struct Desc
    {
        const uint8_t* m_addr;
        uint16_t m_len;
        uint16_t m_numPatches;
        uint16_t m_patches[3][2];
    };

    void Process(Desc& out, const FastInterpBoilerplateBluePrint* blueprint)
    {
        ReleaseAssert(blueprint->m_addr32FixupArrayLength == 1);
        ReleaseAssert(blueprint->m_symbol32FixupArrayLength > 0 && blueprint->m_symbol32FixupArrayLength <= 4);
        ReleaseAssert(blueprint->m_symbol64FixupArrayLength == 0);
        ReleaseAssert(blueprint->m_highestBoilerplateFnptrPlaceholderOrdinal == 1);
        ReleaseAssert(blueprint->m_highestCppFnptrPlaceholderOrdinal == 0);
        ReleaseAssert(UnalignedRead<uint32_t>(blueprint->m_content + blueprint->m_contentLength - 9) == 0x00458949);

        out.m_addr = blueprint->m_content;
        out.m_len = static_cast<uint16_t>(blueprint->m_contentLength - 9);
        out.m_numPatches = static_cast<uint16_t>(blueprint->m_symbol32FixupArrayLength - 1);
        for (uint32_t i = 0; i < blueprint->m_symbol32FixupArrayLength - 1; i++)
        {
            uint32_t offset = blueprint->m_symbol32FixupArray[i].m_offset;
            uint32_t ord = blueprint->m_symbol32FixupArray[i].m_ordinalIntoPlaceholderArray;
            ReleaseAssert(1 <= ord && ord <= 5);
            ord--;
            out.m_patches[i][0] = static_cast<uint16_t>(offset);
            out.m_patches[i][1] = static_cast<uint16_t>(ord);
        }
    }

    Desc m_desc[WasmCommonOpcodeManager::x_maxIntRegs + 1];
};

extern WasmBrTableManager g_wasmBrTableManager;
WasmBrTableManager g_wasmBrTableManager;

class WasmCallIndirectManager
{
public:
    WasmCallIndirectManager()
    {
        for (uint32_t i = 0; i <= WasmCommonOpcodeManager::x_maxIntRegs; i++)
        {
            Process(m_desc[i], FastInterpBoilerplateLibrary<FICallIndirectImpl>::SelectBoilerplateBluePrint(
                        static_cast<FINumOpaqueIntegralParams>((i > 0) ? i - 1 : 0),
                        FIOpaqueParamsHelper::GetMaxOFP(),
                        i > 0));
        }
    }

    void Codegen(uint8_t*& dstAddr, uint32_t numInRegisterInt, uint8_t* ud2Addr, WasmCommonOpcodeFixups* fixups)
    {
        Desc& d = m_desc[numInRegisterInt];
        memcpy(dstAddr, d.m_addr, d.m_len);
        for (uint32_t i = 0; i < 2; i++)
        {
            UnalignedAddAndWriteback<uint32_t>(dstAddr + d.m_addrOffset[i], static_cast<uint32_t>(ud2Addr - dstAddr));
        }
        for (uint16_t i = 0; i < d.m_numPatches; i++)
        {
            UnalignedAddAndWriteback<uint32_t>(dstAddr + d.m_patches[i][0], static_cast<uint32_t>(fixups->m_data[d.m_patches[i][1]]));
        }
        dstAddr += d.m_len;
    }

    void EmitCall(uint8_t*& dstAddr)
    {
        constexpr uint8_t x_inst[9] = { 0x49, 0x8b, 0x45, 0x00, 0x4d, 0x89, 0xf5, 0xff, 0xd0 };
        memcpy(dstAddr, x_inst, 9);
        dstAddr += 9;
    }

private:
    struct Desc
    {
        const uint8_t* m_addr;
        uint16_t m_len;
        uint16_t m_numPatches;
        uint16_t m_addrOffset[2];
        uint16_t m_patches[4][2];
    };

    void Process(Desc& out, const FastInterpBoilerplateBluePrint* blueprint)
    {
        ReleaseAssert(blueprint->m_addr32FixupArrayLength == 3);
        ReleaseAssert(blueprint->m_symbol32FixupArrayLength > 0 &&
                      blueprint->m_symbol32FixupArray[blueprint->m_symbol32FixupArrayLength - 1].m_ordinalIntoPlaceholderArray == 0);
        ReleaseAssert(blueprint->m_symbol64FixupArrayLength == 0);
        ReleaseAssert(blueprint->m_highestBoilerplateFnptrPlaceholderOrdinal == 2);
        ReleaseAssert(blueprint->m_highestCppFnptrPlaceholderOrdinal == 0);
        ReleaseAssert(UnalignedRead<uint32_t>(blueprint->m_content + blueprint->m_contentLength - 9) == 0x00458949);

        out.m_addr = blueprint->m_content;
        out.m_len = static_cast<uint16_t>(blueprint->m_contentLength - 5);
        out.m_numPatches = 0;
        for (uint32_t i = 0; i < 2; i++)
        {
            out.m_addrOffset[i] = static_cast<uint16_t>(blueprint->m_addr32FixupArray[i]);
        }
        for (uint32_t i = 0; i < blueprint->m_symbol32FixupArrayLength - 1; i++)
        {
            uint32_t offset = blueprint->m_symbol32FixupArray[i].m_offset;
            uint32_t ord = blueprint->m_symbol32FixupArray[i].m_ordinalIntoPlaceholderArray;
            if (ord < 2)
            {
                ReleaseAssert(ord == 1);
                continue;
            }
            ord -= 2;
            ReleaseAssert(0 <= ord && ord <= 4);

            out.m_patches[out.m_numPatches][0] = static_cast<uint16_t>(offset);
            out.m_patches[out.m_numPatches][1] = static_cast<uint16_t>(ord);
            out.m_numPatches++;
        }
        ReleaseAssert(out.m_numPatches == static_cast<uint16_t>(blueprint->m_symbol32FixupArrayLength - 3));
    }

    Desc m_desc[WasmCommonOpcodeManager::x_maxIntRegs + 1];
};

extern WasmCallIndirectManager g_wasmCallIndirectManager;
WasmCallIndirectManager g_wasmCallIndirectManager;

class WasmRuntimeMemory : NonMovable, NonCopyable
{
public:
    ~WasmRuntimeMemory()
    {
        if (m_memStart != nullptr)
        {
            assert(m_memZero > m_memStart);
            uint64_t len = static_cast<uint64_t>(m_memZero - m_memStart) + (1ULL << 32);
            munmap(m_memStart, len);
            m_memStart = nullptr;
            m_memZero = nullptr;
        }
    }

    uint64_t GetMemZero() const
    {
        return reinterpret_cast<uint64_t>(m_memZero);
    }

    uint32_t& MemorySizeInPages()
    {
        return *reinterpret_cast<uint32_t*>(m_memZero - 8);
    }

    void SetGs()
    {
        long ret = syscall(__NR_arch_prctl, ARCH_SET_GS, reinterpret_cast<unsigned long>(m_memZero));
        assert(ret == 0);
        std::ignore = ret;

        uint64_t gsLoc = *reinterpret_cast<WasmMemPtr<uint64_t>>(static_cast<uint64_t>(-16));
        assert(reinterpret_cast<uint64_t*>(gsLoc)[-2] == gsLoc);
        assert(gsLoc == reinterpret_cast<uint64_t>(m_memZero));
        std::ignore = gsLoc;
    }

    // May only be called after GS is set
    // Returns the *old* number of pages, or -1 if failed
    //
    static uint32_t GrowMemory(uint32_t numPages)
    {
        // printf("entered growMemory with input %d\n", static_cast<int>(numPages));

        uint64_t gsLoc = *reinterpret_cast<WasmMemPtr<uint64_t>>(static_cast<uint64_t>(-16));
        assert(reinterpret_cast<uint64_t*>(gsLoc)[-2] == gsLoc);
        WasmMemPtr<uint32_t> p = reinterpret_cast<WasmMemPtr<uint32_t>>(static_cast<uint64_t>(-8));
        uint32_t oldNumPages = *p;
        if (numPages == 0)
        {
            return oldNumPages;
        }

        if (static_cast<uint64_t>(oldNumPages) + numPages > (1ULL << 32) / 65536)
        {
            printf("!!!!!!! GrowMemory Failed (invalid param) !!!!!!!\n");
            return static_cast<uint32_t>(-1);
        }
        void* addr = reinterpret_cast<void*>(gsLoc + static_cast<uint64_t>(oldNumPages) * 65536);
        uint64_t len = numPages * 65536;
        void* r = mmap(addr, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE | MAP_FIXED, -1, 0);
        if (r == MAP_FAILED)
        {
            printf("!!!!!!! GrowMemory Failed (OOM) !!!!!!!\n");
            return static_cast<uint32_t>(-1);
        }
        assert(r == addr);
        *p = oldNumPages + numPages;
        return oldNumPages;
    }

    static uint32_t WasmGrowMemoryEntryPoint(uintptr_t operands)
    {
        return GrowMemory(*reinterpret_cast<uint32_t*>(operands + 8));
    }

    static WasmRuntimeMemory* Create(uint64_t negativePartLength, uint32_t numInitPositivePartPages)
    {
        assert(negativePartLength >= 16);
        assert(numInitPositivePartPages >= 0);
        uint64_t alignedNegLen = (negativePartLength + 4095) / 4096 * 4096;
        uint64_t posLen = static_cast<uint64_t>(numInitPositivePartPages) * 65536;
        if (posLen > (1ULL << 32)) { return nullptr; }

        bool success = false;
        void* r = mmap(nullptr, alignedNegLen + (1ULL << 32), PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE | MAP_NONBLOCK, -1, 0);
        if (r == MAP_FAILED)
        {
            return nullptr;
        }
        Auto(if (!success) { munmap(r, alignedNegLen + (1ULL << 32)); });

        void* x = mmap(r, alignedNegLen + posLen, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE | MAP_FIXED, -1, 0);
        if (x == MAP_FAILED)
        {
            return nullptr;
        }
        assert(x == r);

        WasmRuntimeMemory* result = new WasmRuntimeMemory();
        result->m_memStart = reinterpret_cast<uint8_t*>(r);
        result->m_memZero = result->m_memStart + alignedNegLen;

        result->MemorySizeInPages() = numInitPositivePartPages;
        *reinterpret_cast<uint64_t*>(result->m_memZero - 16) = reinterpret_cast<uint64_t>(result->m_memZero);
        // printf("memzero is at 0x%llx\n", reinterpret_cast<unsigned long long>(result->m_memZero));
        success = true;
        return result;
    }

private:
    WasmRuntimeMemory() : m_memStart(nullptr), m_memZero(nullptr) {}

    uint8_t* m_memStart;
    uint8_t* m_memZero;
};

struct WasmDataSection
{
    WasmDataSection()
        : m_numRecords(0)
    { }

    void ParseSection(TempArenaAllocator& alloc, WasmRuntimeMemory* wrm, WasmMemorySection* memorySection, ShallowStream reader)
    {
        assert(wrm->MemorySizeInPages() == 0);
        wrm->GrowMemory(memorySection->m_limit.m_minSize);

        m_numRecords = reader.ReadIntLeb<uint32_t>();
        m_records = new (alloc) WasmDataRecord[m_numRecords];
        for (uint32_t i = 0; i < m_numRecords; i++)
        {
            m_records[i].Parse(reader);

            if (m_records[i].m_offset.m_isInitByGlobal)
            {
                DEBUG_ONLY(printf("[ERROR] Data section offset initialized by global is currently unsupported. "
                       "Codegen will continue, but the generated code will not be runnable.\n");)
            }
            else
            {
                uint32_t offset;
                memcpy(&offset, m_records[i].m_offset.m_initRawBytes, 4);
                assert(offset + m_records[i].m_length <= static_cast<uint64_t>(memorySection->m_limit.m_minSize) * 65536);
                memcpy(reinterpret_cast<void*>(wrm->GetMemZero() + offset), m_records[i].m_contents, m_records[i].m_length);
            }
        }
        assert(!reader.HasMore());
    }

    void ParseEmptySection(WasmRuntimeMemory* wrm, WasmMemorySection* memorySection)
    {
        assert(wrm->MemorySizeInPages() == 0);
        wrm->GrowMemory(memorySection->m_limit.m_minSize);
    }

    uint32_t m_numRecords;
    WasmDataRecord* m_records;
};

struct WasmGeneratedCodeManager
{
    WasmGeneratedCodeManager()
        : m_regionBegin(nullptr)
        , m_curPos(nullptr)
    { }

    ~WasmGeneratedCodeManager()
    {
        if (m_regionBegin != nullptr)
        {
            munmap(m_regionBegin, 1ULL << 31);
        }
    }

    void Init()
    {
        assert(m_regionBegin == nullptr);
        void* r = mmap(nullptr, 1ULL << 31, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE | MAP_NONBLOCK, -1, 0);
        if (r == MAP_FAILED)
        {
            printf("out of memory\n");
            abort();
        }
        m_regionBegin = reinterpret_cast<uint8_t*>(r);
        m_curPos = m_regionBegin;
        m_materializedLimit = m_regionBegin;
    }

    void Reserve(uint32_t size)
    {
        assert(m_curPos <= m_materializedLimit);
        uint32_t curAmount = static_cast<uint32_t>(m_materializedLimit - m_curPos);
        if (curAmount >= size)
        {
            return;
        }
        size -= curAmount;
        size = (size + x_allocationSize - 1) / x_allocationSize * x_allocationSize;

        void* x = mmap(m_materializedLimit, size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE | MAP_FIXED, -1, 0);
        if (x == MAP_FAILED)
        {
            printf("out of memory\n");
            abort();
        }
        assert(x == m_materializedLimit);
        m_materializedLimit += size;
    }

    static constexpr uint32_t x_allocationSize = 262144;
    uint8_t* m_regionBegin;
    uint8_t* m_curPos;
    uint8_t* m_materializedLimit;
};

struct WasmModule
{
    WasmModule()
        : m_runtimeMemory(nullptr)
#ifndef NDEBUG
        , m_initialized(false)
#endif
    { }

    bool WARN_UNUSED ParseModule(const char* file, double* codegenTime = nullptr)
    {
        assert(!m_initialized);
        DEBUG_ONLY(m_initialized = true;)

        CHECK_ERR(m_file.Open(file));

        AutoTimer t(codegenTime);
        ShallowStream reader = m_file.GetShallowStream();

        // Check magic header
        //
        {
            uint32_t magic_header = reader.ReadScalar<uint32_t>();
            if (magic_header != 0x6d736100U)
            {
                REPORT_ERR("Input file '%s' does not seem to be a WASM file. "
                           "Expecting magic header '0x6d736100', got '0x%x'.", file, static_cast<unsigned int>(magic_header));
                return false;
            }
        }
        {
            uint32_t magic_version = reader.ReadScalar<uint32_t>();
            if (magic_version != 1)
            {
                REPORT_ERR("Input file '%s' has an unsupported WASM version number '%u'. Only version 1 is supported.",
                           file, static_cast<unsigned int>(magic_version));
                return false;
            }
        }

        uint8_t lastSectionId = 0;
        bool m_functionSectionProcessed = false;
        bool m_globalSectionProcessed = false;
        bool m_dataSectionProcessed = false;
        while (m_file.HasMore(reader))
        {
            uint8_t sectionId = reader.ReadScalar<uint8_t>();
            assert(sectionId < static_cast<uint8_t>(WasmSectionId::X_END_OF_ENUM));
            uint32_t sectionLength = reader.ReadIntLeb<uint32_t>();
            // printf("found section %d\n", static_cast<int>(sectionId));
            if (sectionId != static_cast<uint8_t>(WasmSectionId::CUSTOM_SECTION))
            {
                // https://webassembly.github.io/spec/core/binary/modules.html#binary-module
                // Custom sections may be inserted at any place in this sequence, while other
                // sections must occur at most once and in the prescribed order.
                //
                assert(sectionId > lastSectionId);

                // special event for skipping function/global section
                //
                if (lastSectionId < static_cast<uint8_t>(WasmSectionId::FUNCTION_SECTION) &&
                    static_cast<uint8_t>(WasmSectionId::FUNCTION_SECTION) < sectionId)
                {
                    m_functionDeclarations.ParseEmptySection(&m_importSection);
                    m_functionSectionProcessed = true;
                }
                if (lastSectionId < static_cast<uint8_t>(WasmSectionId::GLOBAL_SECTION) &&
                    static_cast<uint8_t>(WasmSectionId::GLOBAL_SECTION) < sectionId)
                {
                    m_globalSection.ParseEmptySection(&m_importSection);
                    m_globalSectionProcessed = true;
                }

                // Parse this section
                //
                assert(sectionId < static_cast<uint8_t>(WasmSectionId::X_END_OF_ENUM));
                WasmSectionId sid = static_cast<WasmSectionId>(sectionId);
                ShallowStream sectionReader = reader.GetShallowStreamFromNow(sectionLength);
                switch (sid)
                {
                case WasmSectionId::TYPE_SECTION:
                {
                    m_functionTypeIndices.ParseSection(m_alloc, sectionReader);
                    break;
                }
                case WasmSectionId::IMPORT_SECTION:
                {
                    m_importSection.ParseSection(m_alloc, sectionReader);
                    break;
                }
                case WasmSectionId::FUNCTION_SECTION:
                {
                    m_functionDeclarations.ParseSection(m_alloc, sectionReader, &m_importSection);
                    m_functionSectionProcessed = true;
                    break;
                }
                case WasmSectionId::TABLE_SECTION:
                {
                    m_tableSection.ParseSection(sectionReader);
                    // current WASM spec allows up to 1 table.
                    //
                    AssertImp(m_tableSection.m_hasTable, !m_importSection.IsTableImported());
                    break;
                }
                case WasmSectionId::MEMORY_SECTION:
                {
                    m_memorySection.ParseSection(sectionReader);
                    // current WASM spec allows up to 1 memory.
                    //
                    AssertImp(m_memorySection.m_hasMemory, !m_importSection.IsMemoryImported());
                    break;
                }
                case WasmSectionId::GLOBAL_SECTION:
                {
                    m_globalSection.ParseSection(m_alloc, sectionReader, &m_importSection);
                    m_globalSectionProcessed = true;
                    break;
                }
                case WasmSectionId::EXPORT_SECTION:
                {
                    m_exportSection.ParseSection(m_alloc, sectionReader);
                    break;
                }
                case WasmSectionId::START_SECTION:
                {
                    m_startSection.ParseSection(sectionReader);
                    break;
                }
                case WasmSectionId::ELEMENT_SECTION:
                {
                    m_elementSection.ParseSection(m_alloc, sectionReader);
                    break;
                }
                case WasmSectionId::CODE_SECTION:
                {
                    CodeGen(sectionReader, sectionLength);
                    break;
                }
                case WasmSectionId::DATA_SECTION:
                {
                    assert(m_runtimeMemory != nullptr);
                    m_dataSection.ParseSection(m_alloc, m_runtimeMemory, &m_memorySection, sectionReader);
                    m_dataSectionProcessed = true;
                    break;
                }
                case WasmSectionId::CUSTOM_SECTION:
                    /* fallthrough */
                case WasmSectionId::X_END_OF_ENUM:
                {
                    assert(false);
                }
                }   /* switch sid */

                lastSectionId = sectionId;
            }
            reader.SkipBytes(sectionLength);
        }

        assert(!reader.HasMore());

        if (!m_functionSectionProcessed)
        {
            assert(!m_globalSectionProcessed && !m_dataSectionProcessed);
            m_functionDeclarations.ParseEmptySection(&m_importSection);
        }
        if (!m_globalSectionProcessed)
        {
            assert(!m_dataSectionProcessed);
            m_globalSection.ParseEmptySection(&m_importSection);
        }
        if (!m_dataSectionProcessed)
        {
            assert(m_runtimeMemory != nullptr);
            m_dataSection.ParseEmptySection(m_runtimeMemory, &m_memorySection);
        }
        return true;
    }

    struct OpcodeInfo
    {
        WasmOpcode m_opcode;
        bool m_spillOutput;
    };

    struct BlockInfo
    {
        uint32_t m_numInts;
        uint32_t m_numFloats;
        WasmValueType m_outputType;
        uint32_t m_numRefCount;
        uint32_t* m_endOpcodeAddress;
    };

    struct CodegenBlockInfo
    {
        uint32_t m_numInRegisterInts;
        uint32_t m_numSpilledInts;
        uint32_t m_numInRegisterFloats;
        uint32_t m_numSpilledFloats;
        WasmValueType m_outputType;
        bool m_spillOutput;
        uint32_t m_numRefCount;
        uint8_t** m_brListStart;
        uint8_t** m_brListCur;
        uint8_t* m_elseBr;
        uint8_t* m_startAddress;
    };

    struct OperandStackManager
    {
        OperandStackManager()
        {
            Reset();
        }

        WasmValueType GetStackType(uint32_t ordFromTop) const
        {
            assert(m_numIntegrals + m_numFloats > ordFromTop);
            return *(m_typeStackTop - ordFromTop - 1);
        }

        uint32_t GetStackHeight() const { return m_numIntegrals + m_numFloats; }
        WasmValueType GetStackTopType() const { return GetStackType(0); }
        WasmValueType GetStack2ndTopType() const { return GetStackType(1); }
        WasmValueType GetStack3rdTopType() const { return GetStackType(2); }

        void Reset()
        {
            m_numIntegrals = 0;
            m_numFloats = 0;
            m_intSpillWaterline = 0;
            m_floatSpillWaterline = 0;
            m_maxIntegrals = 0;
            m_maxFloats = 0;
            m_typeStackTop = m_stackTypes;
        }

        void Reset(uint32_t numIntegrals, uint32_t numFloats)
        {
            assert(numIntegrals <= m_numIntegrals && numFloats <= m_numFloats);
            m_numIntegrals = numIntegrals;
            m_numFloats = numFloats;
            m_intSpillWaterline = std::min(m_intSpillWaterline, m_numIntegrals);
            m_floatSpillWaterline = std::min(m_floatSpillWaterline, m_numFloats);
            m_typeStackTop = m_stackTypes + m_numIntegrals + m_numFloats;
        }

        void ConsumeStack(uint32_t numIntConsumes, uint32_t numFloatConsumes)
        {
            assert(m_numIntegrals >= numIntConsumes && m_numFloats >= numFloatConsumes);
#ifndef NDEBUG
            uint32_t cntf = 0, cnti = 0;
            for (uint32_t i = 0; i < numIntConsumes + numFloatConsumes; i++)
            {
                if (WasmValueTypeHelper::IsIntegral(GetStackType(i)))
                {
                    cnti++;
                }
                else
                {
                    cntf++;
                }
            }
            assert(cnti == numIntConsumes);
            assert(cntf == numFloatConsumes);
#endif
            m_numIntegrals -= numIntConsumes;
            m_intSpillWaterline = std::min(m_intSpillWaterline, m_numIntegrals);
            m_numFloats -= numFloatConsumes;
            m_floatSpillWaterline = std::min(m_floatSpillWaterline, m_numFloats);
            m_typeStackTop -= numIntConsumes + numFloatConsumes;
        }

        void PushStack(WasmValueType outputType, OpcodeInfo* parent)
        {
            assert(m_numFloats + m_numIntegrals < x_maxStackSize);
            *m_typeStackTop = outputType;
            m_typeStackTop++;
            if (WasmValueTypeHelper::IsIntegral(outputType))
            {
                m_intStackParent[m_numIntegrals] = parent;
                assert(m_numIntegrals <= m_maxIntegrals);
                m_maxIntegrals += (m_maxIntegrals == m_numIntegrals);
                m_numIntegrals++;
                if (m_intSpillWaterline + WasmCommonOpcodeManager::x_maxIntRegs < m_numIntegrals)
                {
                    m_intStackParent[m_intSpillWaterline]->m_spillOutput = true;
                    m_intSpillWaterline++;
                    assert(m_intSpillWaterline == m_numIntegrals - WasmCommonOpcodeManager::x_maxIntRegs);
                }
            }
            else
            {
                m_floatStackParent[m_numFloats] = parent;
                assert(m_numFloats <= m_maxFloats);
                m_maxFloats += (m_numFloats == m_maxFloats);
                m_numFloats++;
                if (m_floatSpillWaterline + WasmCommonOpcodeManager::x_maxFloatRegs < m_numFloats)
                {
                    m_floatStackParent[m_floatSpillWaterline]->m_spillOutput = true;
                    m_floatSpillWaterline++;
                    assert(m_floatSpillWaterline == m_numFloats - WasmCommonOpcodeManager::x_maxFloatRegs);
                }
            }
        }

        void ForceSpillAll()
        {
            while (m_intSpillWaterline < m_numIntegrals)
            {
                m_intStackParent[m_intSpillWaterline]->m_spillOutput = true;
                m_intSpillWaterline++;
            }
            while (m_floatSpillWaterline < m_numFloats)
            {
                m_floatStackParent[m_floatSpillWaterline]->m_spillOutput = true;
                m_floatSpillWaterline++;
            }
            assert(m_intSpillWaterline == m_numIntegrals && m_floatSpillWaterline == m_numFloats);
        }

        uint32_t m_numIntegrals;
        uint32_t m_numFloats;
        uint32_t m_intSpillWaterline;
        uint32_t m_floatSpillWaterline;
        uint32_t m_maxIntegrals;
        uint32_t m_maxFloats;
        WasmValueType* m_typeStackTop;

        static constexpr size_t x_maxStackSize = 100000;
        WasmValueType m_stackTypes[x_maxStackSize];
        OpcodeInfo* m_intStackParent[x_maxStackSize];
        OpcodeInfo* m_floatStackParent[x_maxStackSize];
    };

    struct CodegenOperandStackManager
    {
        CodegenOperandStackManager(uint64_t intStackBase, uint64_t maxInts, uint64_t maxFloats)
            : m_intStackBase(intStackBase)
            , m_floatStackBase(intStackBase + maxInts * 8)
            , m_floatStackLimit(intStackBase + maxInts * 8 + maxFloats * 8)
            , m_numInRegisterInt(0)
            , m_numInRegisterFloat(0)
        {
            m_fixupData.m_data[0] = m_intStackBase;
            m_fixupData.m_data[1] = m_floatStackBase;
        }

        void ConsumeInts(uint32_t numIntToConsume)
        {
            uint32_t consumedInRegisterInt = std::min(m_numInRegisterInt, numIntToConsume);
            m_numInRegisterInt -= consumedInRegisterInt;
            m_fixupData.m_data[0] -= 8 * (numIntToConsume - consumedInRegisterInt);
            assert(m_fixupData.m_data[0] >= m_intStackBase);
        }

        void ConsumeFloats(uint32_t numFloatToConsume)
        {
            uint32_t consumedInRegisterFloat = std::min(m_numInRegisterFloat, numFloatToConsume);
            m_numInRegisterFloat -= consumedInRegisterFloat;
            m_fixupData.m_data[1] -= 8 * (numFloatToConsume - consumedInRegisterFloat);
            assert(m_fixupData.m_data[1] >= m_floatStackBase);
        }

        void ProduceOutput(WasmValueType outputType, bool spillOutput)
        {
            if (spillOutput)
            {
                if (WasmValueTypeHelper::IsIntegral(outputType))
                {
                    assert(m_numInRegisterInt == 0);
                    m_fixupData.m_data[0] += 8;
                    assert(m_fixupData.m_data[0] <= m_floatStackBase);
                }
                else
                {
                    assert(m_numInRegisterFloat == 0);
                    m_fixupData.m_data[1] += 8;
                    assert(m_fixupData.m_data[1] <= m_floatStackLimit);
                }
            }
            else
            {
                if (WasmValueTypeHelper::IsIntegral(outputType))
                {
                    m_numInRegisterInt++;
                    assert(m_numInRegisterInt <= WasmCommonOpcodeManager::x_maxIntRegs);
                }
                else
                {
                    m_numInRegisterFloat++;
                    assert(m_numInRegisterFloat <= WasmCommonOpcodeManager::x_maxFloatRegs);
                }
            }
        }

        void Reset(uint32_t numInRegisterInts, uint32_t numInRegisterFloats, uint32_t numSpilledInts, uint32_t numSpilledFloats)
        {
            m_numInRegisterInt = numInRegisterInts;
            m_numInRegisterFloat = numInRegisterFloats;
            m_fixupData.m_data[0] = m_intStackBase + numSpilledInts * 8;
            assert(m_fixupData.m_data[0] <= m_floatStackBase);
            m_fixupData.m_data[1] = m_floatStackBase + numSpilledFloats * 8;
            assert(m_fixupData.m_data[1] <= m_floatStackLimit);
        }

        uint64_t m_intStackBase;
        uint64_t m_floatStackBase;
        uint64_t m_floatStackLimit;
        uint32_t m_numInRegisterInt;
        uint32_t m_numInRegisterFloat;
        WasmCommonOpcodeFixups m_fixupData;
    };

    void CodeGen(ShallowStream reader, uint64_t /*sectionLength*/)
    {
        uint32_t numFuncs = reader.ReadIntLeb<uint32_t>();
        assert(numFuncs == m_functionDeclarations.m_numFunctions - m_functionDeclarations.m_numImportedFunctions);
        std::ignore = numFuncs;
        std::vector<WasmValueType> localTypes;

        m_cgMan.Init();
        m_cgMan.Reserve(16 * (m_functionDeclarations.m_numImportedFunctions + 2));

        uint8_t*& codeRegionBegin = m_cgMan.m_regionBegin;
        uint8_t*& curCodePos = m_cgMan.m_curPos;
        UnalignedWrite<uint16_t>(curCodePos, 0x0b0f);
        curCodePos += 2;
        memset(curCodePos, 0x90, 14);
        curCodePos += 14;

        OperandStackManager osm;
        std::vector<uint8_t*> brOffsetPatchArrayVec;
        std::vector<BlockInfo> blockStack;
        std::vector<CodegenBlockInfo> cgBlockStack;
        std::vector<std::pair<uint8_t*, uint32_t>> callStackSizeFixups;
        std::vector<std::pair<uint8_t*, uint32_t>> indirectCallStackSizeFixups;
        std::vector<std::pair<uint8_t*, uint32_t>> callFuncAddressFixups;
        uint64_t brTableBaseOffset = m_globalSection.m_numGlobals * 8 + 16 + m_tableSection.m_limit.m_minSize * 16;
        m_codegenAuxilaryDataTable.clear();
        constexpr uint8_t x_brToCppInst[16] = { 0x4C, 0x89, 0xEF, 0x48, 0xB8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xE0, 0x90 };

        {
            memcpy(curCodePos, x_brToCppInst, 16);
            UnalignedWrite<uint64_t>(curCodePos + 5, reinterpret_cast<uint64_t>(&WasmRuntimeMemory::WasmGrowMemoryEntryPoint));
            curCodePos += 16;
        }

        for (uint32_t curFunc = 0; curFunc < m_functionDeclarations.m_numImportedFunctions; curFunc++)
        {
            WasmFunctionType funcType = m_functionTypeIndices.GetFunctionTypeFromIdx(m_functionDeclarations.m_functionDeclarations[curFunc]);
            m_functionDeclarations.m_functionStackSize[curFunc] = static_cast<uint32_t>(funcType.m_numParams * 8 + 16) / 16 * 16 + 24;
            m_functionDeclarations.m_functionEntryPoint[curFunc] = curCodePos;
            memcpy(curCodePos, x_brToCppInst, 16);
            WasmImportedEntityName name = m_importSection.GetImportedFunctionName(curFunc);
            auto it = g_wasiLinkMapping.find(std::make_pair(std::string(name.m_lv1Name, name.m_lv1Name + name.m_lv1NameLen),
                                                            std::string(name.m_lv2Name, name.m_lv2Name + name.m_lv2NameLen)));
            if (it == g_wasiLinkMapping.end())
            {
                DEBUG_ONLY(printf("[ERROR] Unknown import function name %s.%s. Codegen will continue, but the generated code will not be runnable.\n",
                       std::string(name.m_lv1Name, name.m_lv1Name + name.m_lv1NameLen).c_str(),
                       std::string(name.m_lv2Name, name.m_lv2Name + name.m_lv2NameLen).c_str()));
            }
            else
            {
                UnalignedWrite<uint64_t>(curCodePos + 5, it->second);
            }
            curCodePos += 16;
        }

        assert(curCodePos <= m_cgMan.m_materializedLimit);

        constexpr uint32_t x_bufferIncreaseSize = 32768;
        uint32_t curMaxFuncLen = x_bufferIncreaseSize;
        uint32_t* operandList = reinterpret_cast<uint32_t*>(mmap(nullptr, sizeof(uint32_t) * curMaxFuncLen, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0));
        assert(operandList != MAP_FAILED);
        OpcodeInfo* opcodeList = reinterpret_cast<OpcodeInfo*>(mmap(nullptr, sizeof(OpcodeInfo) * curMaxFuncLen, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0));
        assert(opcodeList != MAP_FAILED);

        for (uint32_t curFunc = m_functionDeclarations.m_numImportedFunctions; curFunc < m_functionDeclarations.m_numFunctions; curFunc++)
        {
            uint32_t fnSize = reader.ReadIntLeb<uint32_t>();

            if (fnSize > curMaxFuncLen)
            {
                uint32_t newLen = (fnSize + x_bufferIncreaseSize - 1) / x_bufferIncreaseSize * x_bufferIncreaseSize;
                operandList = reinterpret_cast<uint32_t*>(mremap(operandList, sizeof(uint32_t) * curMaxFuncLen, sizeof(uint32_t) * newLen, MREMAP_MAYMOVE));
                assert(operandList != MAP_FAILED);
                opcodeList = reinterpret_cast<OpcodeInfo*>(mremap(opcodeList, sizeof(OpcodeInfo) * curMaxFuncLen, sizeof(OpcodeInfo) * newLen, MREMAP_MAYMOVE));
                assert(opcodeList != MAP_FAILED);
                curMaxFuncLen = newLen;
            }

            uint32_t compressedLocalVecLen = reader.ReadIntLeb<uint32_t>();
            localTypes.clear();
            WasmFunctionType curFuncType = m_functionTypeIndices.GetFunctionTypeFromIdx(m_functionDeclarations.m_functionDeclarations[curFunc]);
            localTypes.insert(localTypes.end(), curFuncType.m_types, curFuncType.m_types + curFuncType.m_numParams);
            for (uint32_t i = 0; i < compressedLocalVecLen; i++)
            {
                uint32_t runLength = reader.ReadIntLeb<uint32_t>();
                WasmValueType localType = WasmValueTypeHelper::Parse(reader);
                localTypes.insert(localTypes.end(), runLength /*numElements*/, localType);
            }

            uint32_t maxCodeSize = 16;
            blockStack.clear();
            uint32_t maxBlockRefCount = 0;
            auto updateBlockRef = [&blockStack, &maxBlockRefCount](uint64_t brOperand)
            {
                assert(0 <= brOperand && brOperand < blockStack.size());
                uint64_t h = blockStack.size() - 1 - brOperand;
                blockStack[h].m_numRefCount++;
                maxBlockRefCount++;
            };

            uint32_t* curOperand = operandList;
            OpcodeInfo* curOpcode = opcodeList;
            osm.Reset();
            while (true)
            {
                WasmOpcode op = static_cast<WasmOpcode>(reader.ReadScalar<uint8_t>());
                WasmOpcodeInfo info = g_wasmOpcodeInfoTable.Get(op);
                assert(info.m_isValid);
                uint32_t operand = 0;
                if (info.m_operandKind == WasmOpcodeOperandKind::U32)
                {
                    operand = reader.ReadIntLeb<uint32_t>();
                    *(curOperand++) = operand;
                }
                else if (info.m_operandKind == WasmOpcodeOperandKind::NONE)
                {
                    /* no-op */
                }
                else if (info.m_operandKind == WasmOpcodeOperandKind::CONST)
                {
                    if (op == WasmOpcode::I32_CONST)
                    {
                        int32_t data = reader.ReadIntLeb<int32_t>();
                        *(curOperand++) = static_cast<uint32_t>(data);
                    }
                    else if (op == WasmOpcode::I64_CONST)
                    {
                        int64_t data = reader.ReadIntLeb<int64_t>();
                        UnalignedWrite<int64_t>(reinterpret_cast<uint8_t*>(curOperand), data);
                        curOperand += 2;
                    }
                    else if (op == WasmOpcode::F32_CONST)
                    {
                        uint32_t data = reader.ReadScalar<uint32_t>();
                        *(curOperand++) = data;
                    }
                    else
                    {
                        assert(op == WasmOpcode::F64_CONST);
                        uint64_t data = reader.ReadScalar<uint64_t>();
                        UnalignedWrite<uint64_t>(reinterpret_cast<uint8_t*>(curOperand), data);
                        curOperand += 2;
                    }
                }
                else if (info.m_operandKind == WasmOpcodeOperandKind::MEM_U32_U32)
                {
                    uint32_t unused_align = reader.ReadIntLeb<uint32_t>();
                    std::ignore = unused_align;
                    operand = reader.ReadIntLeb<uint32_t>();
                    *(curOperand++) = operand;
                }
                else if (info.m_operandKind == WasmOpcodeOperandKind::BLOCKTYPE)
                {
                    int64_t val = reader.ReadIntLeb<int64_t>();
                    if (val >= 0)
                    {
                        TestAssert(false && "multi-value extension is currently not supported");
                    }
                    if (val < -4)
                    {
                        operand = static_cast<uint32_t>(WasmValueType::X_END_OF_ENUM);
                    }
                    else
                    {
                        operand = static_cast<uint32_t>(-val) - 1;
                    }
                    if (op == WasmOpcode::IF)
                    {
                        assert(osm.GetStackTopType() == WasmValueType::I32);
                        osm.ConsumeStack(1, 0);
                    }
                    *(curOperand++) = operand;
                    *(curOperand++) = osm.m_numIntegrals;
                    *(curOperand++) = osm.m_numFloats;
                    blockStack.push_back({ osm.m_numIntegrals, osm.m_numFloats, static_cast<WasmValueType>(operand), 0, curOperand });
                    curOperand += 2;
                }
                else
                {
                    assert(info.m_operandKind == WasmOpcodeOperandKind::SPECIAL);
                    if (op == WasmOpcode::CALL_INDIRECT)
                    {
                        operand = reader.ReadIntLeb<uint32_t>();
                        // call_indirect has a trailing 0x00 for no use.
                        //
                        std::ignore = reader.ReadScalar<uint8_t>();
                        *(curOperand++) = operand;
                    }
                    else
                    {
                        assert(op == WasmOpcode::BR_TABLE);
                        uint32_t listLen = reader.ReadIntLeb<uint32_t>();
                        *(curOperand++) = listLen;
                        for (uint32_t i = 0; i <= listLen; i++)
                        {
                            uint32_t value = reader.ReadIntLeb<uint32_t>();
                            *(curOperand++) = value;
                            updateBlockRef(value);
                        }
                        maxCodeSize += 32 * listLen;
                    }
                }

                curOpcode->m_spillOutput = false;
                if (!info.m_isSpecial)
                {
                    maxCodeSize += g_wasmCommonOpcodeManager->GetMaxSizeForOpcode(op);
                    osm.ConsumeStack(info.m_numIntConsumes, info.m_numFloatConsumes);
                    if (info.m_hasOutput)
                    {
                        osm.PushStack(info.m_outputType, curOpcode);
                    }
                }
                else
                {
                    if (op == WasmOpcode::LOCAL_GET)
                    {
                        assert(operand < localTypes.size());
                        WasmValueType valType = localTypes[operand];
                        op = static_cast<WasmOpcode>(static_cast<uint8_t>(WasmOpcode::XX_I32_LOCAL_GET) +
                                                     static_cast<uint8_t>(valType));
                        osm.PushStack(valType, curOpcode);
                        curOperand[-1] = curOperand[-1] * 8 + 8;
                        maxCodeSize += g_wasmCommonOpcodeManager->GetMaxSizeForOpcode(op);
                    }
                    else if (op == WasmOpcode::GLOBAL_GET)
                    {
                        assert(operand < m_globalSection.m_numGlobals);
                        WasmValueType valType = m_globalSection.m_globals[operand].m_valueType;
                        op = static_cast<WasmOpcode>(static_cast<uint8_t>(WasmOpcode::XX_I32_GLOBAL_GET) +
                                                     static_cast<uint8_t>(valType));
                        osm.PushStack(valType, curOpcode);
                        curOperand[-1] = curOperand[-1] * 8 + 24;
                        maxCodeSize += g_wasmCommonOpcodeManager->GetMaxSizeForOpcode(op);
                    }
                    else if (op == WasmOpcode::LOCAL_SET)
                    {
                        assert(operand < localTypes.size());
                        WasmValueType valType = localTypes[operand];
                        assert(valType == osm.GetStackTopType());
                        op = static_cast<WasmOpcode>(static_cast<uint8_t>(WasmOpcode::XX_I32_LOCAL_SET) +
                                                     static_cast<uint8_t>(valType));
                        if (WasmValueTypeHelper::IsIntegral(valType))
                        {
                            osm.ConsumeStack(1, 0);
                        }
                        else
                        {
                            osm.ConsumeStack(0, 1);
                        }
                        curOperand[-1] = curOperand[-1] * 8 + 8;
                        maxCodeSize += g_wasmCommonOpcodeManager->GetMaxSizeForOpcode(op);
                    }
                    else if (op == WasmOpcode::LOCAL_TEE)
                    {
                        assert(operand < localTypes.size());
                        WasmValueType valType = localTypes[operand];
                        assert(valType == osm.GetStackTopType());
                        op = static_cast<WasmOpcode>(static_cast<uint8_t>(WasmOpcode::XX_I32_LOCAL_TEE) +
                                                     static_cast<uint8_t>(valType));
                        if (WasmValueTypeHelper::IsIntegral(valType))
                        {
                            osm.ConsumeStack(1, 0);
                        }
                        else
                        {
                            osm.ConsumeStack(0, 1);
                        }
                        osm.PushStack(valType, curOpcode);
                        curOperand[-1] = curOperand[-1] * 8 + 8;
                        maxCodeSize += g_wasmCommonOpcodeManager->GetMaxSizeForOpcode(op);
                    }
                    else if (op == WasmOpcode::GLOBAL_SET)
                    {
                        assert(operand < m_globalSection.m_numGlobals);
                        assert(m_globalSection.m_globals[operand].m_isMutable);
                        WasmValueType valType = m_globalSection.m_globals[operand].m_valueType;
                        assert(valType == osm.GetStackTopType());
                        op = static_cast<WasmOpcode>(static_cast<uint8_t>(WasmOpcode::XX_I32_GLOBAL_SET) +
                                                     static_cast<uint8_t>(valType));
                        if (WasmValueTypeHelper::IsIntegral(valType))
                        {
                            osm.ConsumeStack(1, 0);
                        }
                        else
                        {
                            osm.ConsumeStack(0, 1);
                        }
                        curOperand[-1] = curOperand[-1] * 8 + 24;
                        maxCodeSize += g_wasmCommonOpcodeManager->GetMaxSizeForOpcode(op);
                    }
                    else if (op == WasmOpcode::END)
                    {
                        if (blockStack.size() == 0)
                        {
                            break;
                        }
                        blockStack.back().m_endOpcodeAddress[0] = static_cast<uint32_t>(curOpcode - opcodeList);
                        blockStack.back().m_endOpcodeAddress[1] = blockStack.back().m_numRefCount;
                        osm.Reset(blockStack.back().m_numInts, blockStack.back().m_numFloats);
                        if (blockStack.back().m_outputType != WasmValueType::X_END_OF_ENUM)
                        {
                            osm.PushStack(blockStack.back().m_outputType, curOpcode);
                        }
                        blockStack.pop_back();
                        maxCodeSize += 32;
                    }
                    else if (op == WasmOpcode::BR_IF)
                    {
                        assert(osm.GetStackTopType() == WasmValueType::I32);
                        osm.ConsumeStack(1, 0);
                        updateBlockRef(operand);
                        maxCodeSize += 64;
                    }
                    else if (op == WasmOpcode::DROP)
                    {
                        assert(osm.GetStackHeight() > 0);
                        if (WasmValueTypeHelper::IsIntegral(osm.GetStackTopType()))
                        {
                            op = WasmOpcode::XX_I_DROP;
                            osm.ConsumeStack(1, 0);
                        }
                        else
                        {
                            op = WasmOpcode::XX_F_DROP;
                            osm.ConsumeStack(0, 1);
                        }
                    }
                    else if (op == WasmOpcode::BLOCK)
                    {
                        /* no-op */
                    }
                    else if (op == WasmOpcode::CALL)
                    {
                        assert(operand < m_functionDeclarations.m_numFunctions);
                        uint32_t calleeTypeIdx = m_functionDeclarations.m_functionDeclarations[operand];
                        WasmFunctionType fnType = m_functionTypeIndices.GetFunctionTypeFromIdx(calleeTypeIdx);
#ifndef NDEBUG
                        assert(osm.GetStackHeight() >= fnType.m_numParams);
                        for (uint32_t i = 0; i < fnType.m_numParams; i++)
                        {
                            assert(fnType.m_types[i] == osm.GetStackType(fnType.m_numParams - i - 1));
                        }
#endif
                        osm.ConsumeStack(fnType.m_numIntParams, fnType.m_numFloatParams);

                        osm.ForceSpillAll();

                        if (fnType.m_numReturns > 0)
                        {
                            assert(fnType.m_numReturns == 1);
                            osm.PushStack(fnType.GetReturnType(0), curOpcode);
                        }

                        maxCodeSize += 64 + 16 * fnType.m_numParams;
                    }
                    else if (op == WasmOpcode::IF)
                    {
                        maxCodeSize += 32;
                    }
                    else if (op == WasmOpcode::ELSE)
                    {
                        assert(blockStack.size() > 0);
                        osm.Reset(blockStack.back().m_numInts, blockStack.back().m_numFloats);
                    }
                    else if (op == WasmOpcode::BR)
                    {
                        updateBlockRef(operand);
                        maxCodeSize += 32;
                    }
                    else if (op == WasmOpcode::LOOP)
                    {
                        maxCodeSize += 32;
                    }
                    else if (op == WasmOpcode::SELECT)
                    {
                        assert(osm.GetStackHeight() >= 3);
                        WasmValueType valType = osm.GetStack2ndTopType();
                        assert(osm.GetStackTopType() == WasmValueType::I32 && osm.GetStack3rdTopType() == valType);
                        op = static_cast<WasmOpcode>(static_cast<uint8_t>(WasmOpcode::XX_I32_SELECT) +
                                                     static_cast<uint8_t>(valType));
                        if (WasmValueTypeHelper::IsIntegral(valType))
                        {
                            osm.ConsumeStack(3, 0);
                        }
                        else
                        {
                           osm.ConsumeStack(1, 2);
                        }
                        osm.PushStack(valType, curOpcode);
                        maxCodeSize += g_wasmCommonOpcodeManager->GetMaxSizeForOpcode(op);
                    }
                    else if (op == WasmOpcode::UNREACHABLE || op == WasmOpcode::NOP)
                    {
                        maxCodeSize += 2;
                    }
                    else if (op == WasmOpcode::RETURN)
                    {
                        if (curFuncType.m_numReturns == 0)
                        {
                            op = WasmOpcode::XX_NONE_RETURN;
                        }
                        else
                        {
                            assert(curFuncType.m_numReturns == 1);
                            WasmValueType returnType = curFuncType.GetReturnType(0);
                            assert(osm.GetStackHeight() > 0 && osm.GetStackTopType() == returnType);
                            op = static_cast<WasmOpcode>(static_cast<uint8_t>(WasmOpcode::XX_I32_RETURN) +
                                                         static_cast<uint8_t>(returnType));
                        }
                        maxCodeSize += g_wasmCommonOpcodeManager->GetMaxSizeForOpcode(op);
                    }
                    else if (op == WasmOpcode::CALL_INDIRECT)
                    {
                        assert(osm.GetStackHeight() > 0 && osm.GetStackTopType() == WasmValueType::I32);
                        osm.ConsumeStack(1, 0);

                        WasmFunctionType fnType = m_functionTypeIndices.GetFunctionTypeFromIdx(static_cast<uint32_t>(operand));
#ifndef NDEBUG
                        assert(osm.GetStackHeight() >= fnType.m_numParams);
                        for (uint32_t i = 0; i < fnType.m_numParams; i++)
                        {
                            assert(fnType.m_types[i] == osm.GetStackType(fnType.m_numParams - i - 1));
                        }
#endif
                        osm.ConsumeStack(fnType.m_numIntParams, fnType.m_numFloatParams);

                        osm.ForceSpillAll();

                        if (fnType.m_numReturns > 0)
                        {
                            assert(fnType.m_numReturns == 1);
                            osm.PushStack(fnType.GetReturnType(0), curOpcode);
                        }

                        maxCodeSize += 64 + 16 * fnType.m_numParams;
                    }
                    else if (op == WasmOpcode::BR_TABLE)
                    {
                        assert(osm.GetStackHeight() > 0 && osm.GetStackTopType() == WasmValueType::I32);
                        /* block stack already updated in decoder branch */
                        maxCodeSize += 64;
                    }
                    else if (op == WasmOpcode::MEMORY_SIZE)
                    {
                        osm.PushStack(WasmValueType::I32, curOpcode);
                        maxCodeSize += 64;
                    }
                    else if (op == WasmOpcode::MEMORY_GROW)
                    {
                        osm.ConsumeStack(1, 0);
                        osm.ForceSpillAll();
                        osm.PushStack(WasmValueType::I32, curOpcode);
                        maxCodeSize += 64;
                    }
                    else
                    {
                        assert(false && "unhandled opcode");
                    }
                }

                if (op == WasmOpcode::UNREACHABLE || op == WasmOpcode::BR || op == WasmOpcode::BR_TABLE || op == WasmOpcode::RETURN)
                {
                    assert(reader.PeekScalar<uint8_t>() == static_cast<uint8_t>(WasmOpcode::END) ||
                           reader.PeekScalar<uint8_t>() == static_cast<uint8_t>(WasmOpcode::UNREACHABLE) ||
                           reader.PeekScalar<uint8_t>() == static_cast<uint8_t>(WasmOpcode::ELSE));
                }

                curOpcode->m_opcode = op;
                curOpcode++;
            }
            assert(blockStack.empty());

            if (curOpcode == opcodeList ||
                (!(WasmOpcode::XX_I32_RETURN <= curOpcode[-1].m_opcode && curOpcode[-1].m_opcode <= WasmOpcode::XX_NONE_RETURN)
                && curOpcode[-1].m_opcode != WasmOpcode::UNREACHABLE))
            {
                if (curFuncType.m_numReturns == 0)
                {
                    curOpcode->m_opcode = WasmOpcode::XX_NONE_RETURN;
                }
                else
                {
                    assert(curFuncType.m_numReturns == 1);
                    WasmValueType returnType = curFuncType.GetReturnType(0);
                    assert(osm.GetStackHeight() > 0 && osm.GetStackTopType() == returnType);
                    curOpcode->m_opcode = static_cast<WasmOpcode>(static_cast<uint8_t>(WasmOpcode::XX_I32_RETURN) +
                                                                  static_cast<uint8_t>(returnType));
                }
                curOpcode++;
                maxCodeSize += 32;
            }

            assert(curOpcode <= opcodeList + curMaxFuncLen);
            assert(curOperand <= operandList + curMaxFuncLen);

            if (maxBlockRefCount > brOffsetPatchArrayVec.size())
            {
                brOffsetPatchArrayVec.resize(maxBlockRefCount);
            }

            m_cgMan.Reserve(maxCodeSize);

            {
                constexpr uint64_t x_codeAlign = 16;
                uint64_t rm = reinterpret_cast<uint64_t>(curCodePos) % x_codeAlign;
                if (rm != 0)
                {
                    x86_64_populate_NOP_instructions(curCodePos, x_codeAlign - rm);
                    curCodePos += x_codeAlign - rm;
                    assert(reinterpret_cast<uint64_t>(curCodePos) % x_codeAlign == 0);
                }
            }

            uint8_t** brOffsetPatchArray = brOffsetPatchArrayVec.data();
            uint8_t** curBrOffsetListPos = brOffsetPatchArray;
            OpcodeInfo* opcodeEnd = curOpcode;
            curOpcode = opcodeList;
            uint32_t* operandEnd = curOperand;
            std::ignore = operandEnd;
            curOperand = operandList;
            CodegenOperandStackManager cgOsm(localTypes.size() * 8 + 8 /*intStackBase*/,
                                             osm.m_maxIntegrals /*maxInts*/,
                                             osm.m_maxFloats /*maxFloats*/);
            m_functionDeclarations.m_functionStackSize[curFunc] = static_cast<uint32_t>(cgOsm.m_floatStackLimit + 8) / 16 * 16 + 24;
            m_functionDeclarations.m_functionEntryPoint[curFunc] = curCodePos;
            cgBlockStack.clear();

            // In wasm, all local variables are initialized to 0
            // Emit instruction to zero out local variables
            //
            {
                uint32_t numToZeroOut = static_cast<uint32_t>(localTypes.size() - curFuncType.m_numParams);
                uint32_t offset = curFuncType.m_numParams * 8 + 8;
                if (numToZeroOut > 1)
                {
                    // vxorps %xmm0,%xmm0,%xmm0
                    UnalignedWrite<uint32_t>(curCodePos, 0xc057f8c5U);
                    curCodePos += 4;
                    constexpr uint8_t x_movupsInstr[5] = { 0xc4, 0xc1, 0x78, 0x11, 0x85 };
                    while (numToZeroOut > 1)
                    {
                        memcpy(curCodePos, x_movupsInstr, 5);
                        UnalignedWrite<uint32_t>(curCodePos + 5, offset);
                        curCodePos += 9;
                        numToZeroOut -= 2;
                        offset += 16;
                    }
                }
                if (numToZeroOut > 0)
                {
                    assert(numToZeroOut == 1);
                    constexpr uint8_t x_movqInstr[11] = {0x49, 0xc7, 0x85, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
                    memcpy(curCodePos, x_movqInstr, 11);
                    UnalignedWrite<uint32_t>(curCodePos + 3, offset);
                    curCodePos += 11;
                }
            }

            while (curOpcode < opcodeEnd)
            {
                WasmOpcode op = curOpcode->m_opcode;
                WasmOpcodeInfo opcodeInfo = g_wasmOpcodeInfoTable.Get(op);
                if (likely(!opcodeInfo.m_isSpecial))
                {
                    WasmCommonOpcodeStencil* stencil = g_wasmCommonOpcodeManager->Get(op, cgOsm.m_numInRegisterInt, cgOsm.m_numInRegisterFloat, curOpcode->m_spillOutput);
                    if (opcodeInfo.m_operandKind != WasmOpcodeOperandKind::NONE)
                    {
                        if (unlikely(op == WasmOpcode::I64_CONST || op == WasmOpcode::F64_CONST))
                        {
                            cgOsm.m_fixupData.m_data[2] = UnalignedRead<uint64_t>(reinterpret_cast<uint8_t*>(curOperand));
                            curOperand += 2;
                        }
                        else
                        {
                            cgOsm.m_fixupData.m_data[2] = *curOperand;
                            curOperand++;
                        }
                    }
                    stencil->Fixup(curCodePos, &cgOsm.m_fixupData);
                    cgOsm.ConsumeInts(opcodeInfo.m_numIntConsumes);
                    cgOsm.ConsumeFloats(opcodeInfo.m_numFloatConsumes);
                    if (opcodeInfo.m_hasOutput)
                    {
                        cgOsm.ProduceOutput(opcodeInfo.m_outputType, curOpcode->m_spillOutput);
                    }
                }
                else
                {
                    if (op == WasmOpcode::END)
                    {
                        assert(cgBlockStack.size() > 0);
                        const CodegenBlockInfo& bi = cgBlockStack.back();

                        {
                            assert(bi.m_brListCur == bi.m_brListStart + bi.m_numRefCount);
                            assert(curBrOffsetListPos == bi.m_brListCur);
                            uint32_t value;
                            if (bi.m_startAddress != nullptr)
                            {
                                // this is a loop, a 'br' to this block jumps to the start of the loop
                                //
                                value = static_cast<uint32_t>(reinterpret_cast<uint64_t>(bi.m_startAddress));
                            }
                            else
                            {
                                // this is not a loop, a 'br' to this block jumps to the end of the block
                                //
                                value = static_cast<uint32_t>(reinterpret_cast<uint64_t>(curCodePos));
                            }

                            uint8_t** cur = bi.m_brListStart;
                            while (cur < bi.m_brListCur)
                            {
                                UnalignedAddAndWriteback<uint32_t>(*cur, value);
                                cur++;
                            }

                            if (bi.m_elseBr != nullptr)
                            {
                                // this is a if branch without else clause, fix the conditional jmp instruction
                                //
                                UnalignedAddAndWriteback<uint32_t>(bi.m_elseBr, value);
                                assert(bi.m_startAddress == nullptr);
                            }
                        }

                        curBrOffsetListPos -= bi.m_numRefCount;
                        assert(curBrOffsetListPos >= brOffsetPatchArray);

                        cgOsm.Reset(bi.m_numInRegisterInts, bi.m_numInRegisterFloats, bi.m_numSpilledInts, bi.m_numSpilledFloats);
                        if (bi.m_outputType != WasmValueType::X_END_OF_ENUM)
                        {
                            cgOsm.ProduceOutput(bi.m_outputType, bi.m_spillOutput);
                        }
                        cgBlockStack.pop_back();
                    }
                    else if (op == WasmOpcode::BR_IF)
                    {
                        uint32_t operand = *curOperand;
                        curOperand++;
                        assert(operand < cgBlockStack.size());
                        CodegenBlockInfo& bi = cgBlockStack[cgBlockStack.size() - 1 - operand];

                        uint8_t* patchLoc;
                        if (bi.m_outputType != WasmValueType::X_END_OF_ENUM)
                        {
                            if (bi.m_spillOutput)
                            {
                                if (WasmValueTypeHelper::IsIntegral(bi.m_outputType))
                                {
                                    cgOsm.m_fixupData.m_data[2] = cgOsm.m_intStackBase + bi.m_numSpilledInts * 8 + 8;
                                    assert(cgOsm.m_fixupData.m_data[2] <= cgOsm.m_floatStackBase);
                                }
                                else
                                {
                                    cgOsm.m_fixupData.m_data[2] = cgOsm.m_floatStackBase + bi.m_numSpilledFloats * 8 + 8;
                                    assert(cgOsm.m_fixupData.m_data[2] <= cgOsm.m_floatStackLimit);
                                }
                            }
                            patchLoc = g_wasmBranchManager->CodegenCondBranchWithOutput(
                                        curCodePos,
                                        cgOsm.m_numInRegisterInt,
                                        cgOsm.m_numInRegisterFloat,
                                        bi.m_numInRegisterInts,
                                        bi.m_numInRegisterFloats,
                                        bi.m_outputType,
                                        bi.m_spillOutput,
                                        &cgOsm.m_fixupData);
                        }
                        else
                        {
                            patchLoc = g_wasmBranchManager->CodegenCondBranchWithoutOutput(
                                        curCodePos,
                                        cgOsm.m_numInRegisterInt,
                                        &cgOsm.m_fixupData);
                        }

                        cgOsm.ConsumeInts(1);

                        assert(bi.m_brListCur < bi.m_brListStart + bi.m_numRefCount);
                        *bi.m_brListCur = patchLoc;
                        bi.m_brListCur++;
                    }
                    else if (op == WasmOpcode::BLOCK || op == WasmOpcode::IF || op == WasmOpcode::LOOP)
                    {
                        uint8_t* startAddress = nullptr;
                        uint8_t* elseBr = nullptr;
                        if (op == WasmOpcode::IF)
                        {
                            elseBr = g_wasmBranchManager->CodegenIfBranch(curCodePos, cgOsm.m_numInRegisterInt, &cgOsm.m_fixupData);
                            cgOsm.ConsumeInts(1);
                        }
                        else if (op == WasmOpcode::LOOP)
                        {

                            constexpr uint64_t x_codeAlign = 16;
                            uint64_t rm = reinterpret_cast<uint64_t>(curCodePos) % x_codeAlign;
                            if (rm != 0)
                            {
                                x86_64_populate_NOP_instructions(curCodePos, x_codeAlign - rm);
                                curCodePos += x_codeAlign - rm;
                                assert(reinterpret_cast<uint64_t>(curCodePos) % x_codeAlign == 0);
                            }

                            startAddress = curCodePos;
                        }

                        WasmValueType outputType = static_cast<WasmValueType>(curOperand[0]);
                        uint32_t numTotalInt = static_cast<uint32_t>(curOperand[1]);
                        uint32_t curSpilledInt = static_cast<uint32_t>((cgOsm.m_fixupData.m_data[0] - cgOsm.m_intStackBase) / 8);
                        uint32_t blockSpilledInt, blockInRegisterInt;
                        assert(numTotalInt <= curSpilledInt + cgOsm.m_numInRegisterInt);
                        if (numTotalInt <= curSpilledInt)
                        {
                            blockSpilledInt = numTotalInt;
                            blockInRegisterInt = 0;
                        }
                        else
                        {
                            blockSpilledInt = curSpilledInt;
                            blockInRegisterInt = numTotalInt - curSpilledInt;
                        }
                        uint32_t numTotalFloat = static_cast<uint32_t>(curOperand[2]);
                        uint32_t curSpilledFloat = static_cast<uint32_t>((cgOsm.m_fixupData.m_data[1] - cgOsm.m_floatStackBase) / 8);
                        uint32_t blockSpilledFloat, blockInRegisterFloat;
                        assert(numTotalFloat <= curSpilledFloat + cgOsm.m_numInRegisterFloat);
                        if (numTotalFloat <= curSpilledFloat)
                        {
                            blockSpilledFloat = numTotalFloat;
                            blockInRegisterFloat = 0;
                        }
                        else
                        {
                            blockSpilledFloat = curSpilledFloat;
                            blockInRegisterFloat = numTotalFloat - curSpilledFloat;
                        }
                        OpcodeInfo* endOpcode = opcodeList + curOperand[3];
                        assert(endOpcode->m_opcode == WasmOpcode::END);
                        bool spillOutput = endOpcode->m_spillOutput;

                        uint32_t numRefcount = static_cast<uint32_t>(curOperand[4]);

                        cgBlockStack.push_back(CodegenBlockInfo {
                            blockInRegisterInt,
                            blockSpilledInt,
                            blockInRegisterFloat,
                            blockSpilledFloat,
                            outputType,
                            spillOutput,
                            numRefcount,
                            curBrOffsetListPos,
                            curBrOffsetListPos,
                            elseBr,
                            startAddress
                        });

                        curBrOffsetListPos += numRefcount;
                        assert(curBrOffsetListPos <= brOffsetPatchArray + maxBlockRefCount);

                        curOperand += 5;
                    }
                    else if (op == WasmOpcode::CALL)
                    {
                        uint32_t operand = *curOperand;
                        curOperand++;
                        assert(operand < m_functionDeclarations.m_numFunctions);
                        uint32_t calleeTypeIdx = m_functionDeclarations.m_functionDeclarations[operand];
                        WasmFunctionType fnType = m_functionTypeIndices.GetFunctionTypeFromIdx(calleeTypeIdx);
                        WasmValueType returnType = WasmValueType::X_END_OF_ENUM;
                        if (fnType.m_numReturns > 0)
                        {
                            assert(fnType.m_numReturns == 1);
                            returnType = fnType.GetReturnType(0);
                        }
                        uint8_t* fnStackSizeAddr1 = g_wasmCallManager.EmitPrepare(curCodePos, returnType, curOpcode->m_spillOutput);
                        cgOsm.m_fixupData.m_data[2] = 8 * fnType.m_numParams;
                        for (uint32_t i = 0; i < fnType.m_numParams; i++)
                        {
                            WasmValueType paramType = fnType.GetParamType(fnType.m_numParams - 1 - i);
                            g_wasmCommonOpcodeManager->Get(static_cast<WasmOpcode>(static_cast<uint8_t>(WasmOpcode::XX_I32_FILLPARAM)
                                                                                   + static_cast<uint8_t>(paramType)),
                                                           cgOsm.m_numInRegisterInt, cgOsm.m_numInRegisterFloat, false)
                                    ->Fixup(curCodePos, &cgOsm.m_fixupData);
                            if (WasmValueTypeHelper::IsIntegral(paramType))
                            {
                                cgOsm.ConsumeInts(1);
                            }
                            else
                            {
                                cgOsm.ConsumeFloats(1);
                            }
                            cgOsm.m_fixupData.m_data[2] -= 8;
                        }
                        assert(cgOsm.m_numInRegisterInt == 0);
                        assert(cgOsm.m_numInRegisterFloat == 0);
                        g_wasmCommonOpcodeManager->Get(WasmOpcode::XX_SWITCH_SF, 0, 0, false)->Fixup(curCodePos, &cgOsm.m_fixupData);
                        uint8_t* fnAddr = g_wasmCallManager.EmitCall(curCodePos);
                        uint8_t* fnStackSizeAddr2 = g_wasmCallManager.EmitCleanup(curCodePos, returnType, curOpcode->m_spillOutput, &cgOsm.m_fixupData);
                        if (fnType.m_numReturns > 0)
                        {
                            assert(fnType.m_numReturns == 1);
                            cgOsm.ProduceOutput(fnType.GetReturnType(0), curOpcode->m_spillOutput);
                        }

                        callStackSizeFixups.push_back(std::make_pair(fnStackSizeAddr1, operand));
                        callStackSizeFixups.push_back(std::make_pair(fnStackSizeAddr2, operand));
                        callFuncAddressFixups.push_back(std::make_pair(fnAddr, operand));
                    }
                    else if (op == WasmOpcode::ELSE)
                    {
                        assert(cgBlockStack.size() > 0 && cgBlockStack.back().m_elseBr != nullptr);
                        CodegenBlockInfo& bi = cgBlockStack.back();
                        UnalignedAddAndWriteback<uint32_t>(bi.m_elseBr, static_cast<uint32_t>(reinterpret_cast<uint64_t>(curCodePos)));
                        bi.m_elseBr = nullptr;

                        cgOsm.Reset(bi.m_numInRegisterInts, bi.m_numInRegisterFloats, bi.m_numSpilledInts, bi.m_numSpilledFloats);
                    }
                    else if (op == WasmOpcode::BR)
                    {
                        uint32_t operand = *curOperand;
                        curOperand++;
                        assert(operand < cgBlockStack.size());
                        CodegenBlockInfo& bi = cgBlockStack[cgBlockStack.size() - 1 - operand];

                        uint8_t* patchLoc;
                        if (bi.m_outputType != WasmValueType::X_END_OF_ENUM)
                        {
                            // TODO: if the jump target is a loop, does it have output or not??
                            if (bi.m_spillOutput)
                            {
                                if (WasmValueTypeHelper::IsIntegral(bi.m_outputType))
                                {
                                    cgOsm.m_fixupData.m_data[2] = cgOsm.m_intStackBase + bi.m_numSpilledInts * 8 + 8;
                                    assert(cgOsm.m_fixupData.m_data[2] <= cgOsm.m_floatStackBase);
                                }
                                else
                                {
                                    cgOsm.m_fixupData.m_data[2] = cgOsm.m_floatStackBase + bi.m_numSpilledFloats * 8 + 8;
                                    assert(cgOsm.m_fixupData.m_data[2] <= cgOsm.m_floatStackLimit);
                                }
                            }
                            patchLoc = g_wasmBranchManager->CodegenBranchWithOutput(
                                        curCodePos,
                                        cgOsm.m_numInRegisterInt,
                                        cgOsm.m_numInRegisterFloat,
                                        bi.m_numInRegisterInts,
                                        bi.m_numInRegisterFloats,
                                        bi.m_outputType,
                                        bi.m_spillOutput,
                                        &cgOsm.m_fixupData);
                        }
                        else
                        {
                            patchLoc = g_wasmBranchManager->CodegenBranchWithoutOutput(curCodePos);
                        }

                        assert(bi.m_brListCur < bi.m_brListStart + bi.m_numRefCount);
                        *bi.m_brListCur = patchLoc;
                        bi.m_brListCur++;
                    }
                    else if (op == WasmOpcode::UNREACHABLE)
                    {
                        UnalignedWrite<uint16_t>(curCodePos, 0x0b0f);
                        curCodePos += 2;
                    }
                    else if (op == WasmOpcode::NOP)
                    {

                    }
                    else if (op == WasmOpcode::CALL_INDIRECT)
                    {
                        uint32_t operand = *curOperand;
                        curOperand++;
                        assert(operand < m_functionTypeIndices.GetNumFunctionTypes());

                        cgOsm.m_fixupData.m_data[3] = m_tableSection.m_limit.m_minSize;
                        cgOsm.m_fixupData.m_data[2] = -brTableBaseOffset;
                        cgOsm.m_fixupData.m_data[4] = operand;

                        g_wasmCallIndirectManager.Codegen(curCodePos, cgOsm.m_numInRegisterInt, codeRegionBegin, &cgOsm.m_fixupData);

                        cgOsm.ConsumeInts(1);

                        WasmFunctionType fnType = m_functionTypeIndices.GetFunctionTypeFromIdx(static_cast<uint32_t>(operand));

                        WasmValueType returnType = WasmValueType::X_END_OF_ENUM;
                        if (fnType.m_numReturns > 0)
                        {
                            assert(fnType.m_numReturns == 1);
                            returnType = fnType.GetReturnType(0);
                        }
                        uint8_t* fnStackSizeAddr1 = g_wasmCallManager.EmitPrepare(curCodePos, returnType, curOpcode->m_spillOutput);
                        cgOsm.m_fixupData.m_data[2] = 8 * fnType.m_numParams;
                        for (uint32_t i = 0; i < fnType.m_numParams; i++)
                        {
                            WasmValueType paramType = fnType.GetParamType(fnType.m_numParams - 1 - i);
                            g_wasmCommonOpcodeManager->Get(static_cast<WasmOpcode>(static_cast<uint8_t>(WasmOpcode::XX_I32_FILLPARAM)
                                                                                   + static_cast<uint8_t>(paramType)),
                                                           cgOsm.m_numInRegisterInt, cgOsm.m_numInRegisterFloat, false)
                                    ->Fixup(curCodePos, &cgOsm.m_fixupData);
                            if (WasmValueTypeHelper::IsIntegral(paramType))
                            {
                                cgOsm.ConsumeInts(1);
                            }
                            else
                            {
                                cgOsm.ConsumeFloats(1);
                            }
                            cgOsm.m_fixupData.m_data[2] -= 8;
                        }
                        assert(cgOsm.m_numInRegisterInt == 0);
                        assert(cgOsm.m_numInRegisterFloat == 0);
                        g_wasmCallIndirectManager.EmitCall(curCodePos);
                        uint8_t* fnStackSizeAddr2 = g_wasmCallManager.EmitCleanup(curCodePos, returnType, curOpcode->m_spillOutput, &cgOsm.m_fixupData);
                        if (fnType.m_numReturns > 0)
                        {
                            assert(fnType.m_numReturns == 1);
                            cgOsm.ProduceOutput(fnType.GetReturnType(0), curOpcode->m_spillOutput);
                        }

                        indirectCallStackSizeFixups.push_back(std::make_pair(fnStackSizeAddr1, operand));
                        indirectCallStackSizeFixups.push_back(std::make_pair(fnStackSizeAddr2, operand));
                    }
                    else if (op == WasmOpcode::BR_TABLE)
                    {
                        uint32_t listLen = *curOperand;
                        curOperand++;

                        uint64_t baseOffset = brTableBaseOffset + m_codegenAuxilaryDataTable.size() * 8;
                        baseOffset += (listLen + 1) * 8;
                        baseOffset = -baseOffset;
                        m_codegenAuxilaryDataTable.insert(m_codegenAuxilaryDataTable.end(), listLen + 1 /*numValues*/, 0);
                        uint64_t* curValueToFill = &m_codegenAuxilaryDataTable.back();

                        cgOsm.m_fixupData.m_data[2] = baseOffset;
                        cgOsm.m_fixupData.m_data[3] = listLen;
                        g_wasmBrTableManager.Codegen(curCodePos, cgOsm.m_numInRegisterInt, &cgOsm.m_fixupData);

                        cgOsm.ConsumeInts(1);

                        for (uint64_t i = 0; i <= listLen; i++)
                        {
                            *curValueToFill = reinterpret_cast<uint64_t>(curCodePos);
                            curValueToFill--;

                            uint32_t operand = *curOperand;
                            curOperand++;

                            assert(operand < cgBlockStack.size());
                            CodegenBlockInfo& bi = cgBlockStack[cgBlockStack.size() - 1 - operand];

                            uint8_t* patchLoc;
                            if (bi.m_outputType != WasmValueType::X_END_OF_ENUM)
                            {
                                if (bi.m_spillOutput)
                                {
                                    if (WasmValueTypeHelper::IsIntegral(bi.m_outputType))
                                    {
                                        cgOsm.m_fixupData.m_data[2] = cgOsm.m_intStackBase + bi.m_numSpilledInts * 8 + 8;
                                        assert(cgOsm.m_fixupData.m_data[2] <= cgOsm.m_floatStackBase);
                                    }
                                    else
                                    {
                                        cgOsm.m_fixupData.m_data[2] = cgOsm.m_floatStackBase + bi.m_numSpilledFloats * 8 + 8;
                                        assert(cgOsm.m_fixupData.m_data[2] <= cgOsm.m_floatStackLimit);
                                    }
                                }
                                patchLoc = g_wasmBranchManager->CodegenBranchWithOutput(
                                            curCodePos,
                                            cgOsm.m_numInRegisterInt,
                                            cgOsm.m_numInRegisterFloat,
                                            bi.m_numInRegisterInts,
                                            bi.m_numInRegisterFloats,
                                            bi.m_outputType,
                                            bi.m_spillOutput,
                                            &cgOsm.m_fixupData);
                            }
                            else
                            {
                                patchLoc = g_wasmBranchManager->CodegenBranchWithoutOutput(curCodePos);
                            }

                            assert(bi.m_brListCur < bi.m_brListStart + bi.m_numRefCount);
                            *bi.m_brListCur = patchLoc;
                            bi.m_brListCur++;
                        }
                    }
                    else if (op == WasmOpcode::MEMORY_SIZE)
                    {
                        cgOsm.m_fixupData.m_data[2] = 8;
                        g_wasmCommonOpcodeManager->Get(
                                    WasmOpcode::XX_I32_GLOBAL_GET, cgOsm.m_numInRegisterInt, cgOsm.m_numInRegisterFloat, curOpcode->m_spillOutput)
                                ->Fixup(curCodePos, &cgOsm.m_fixupData);
                        cgOsm.ProduceOutput(WasmValueType::I32, curOpcode->m_spillOutput);
                        curOperand++;
                    }
                    else if (op == WasmOpcode::MEMORY_GROW)
                    {
                        uint8_t* tmp = curCodePos;
                        std::ignore = tmp;
                        uint8_t* fnStackSizeAddr1 = g_wasmCallManager.EmitPrepare(curCodePos, WasmValueType::I32, curOpcode->m_spillOutput);
                        UnalignedWrite<uint32_t>(fnStackSizeAddr1, 40);
                        cgOsm.m_fixupData.m_data[2] = 8;
                        g_wasmCommonOpcodeManager->Get(WasmOpcode::XX_I32_FILLPARAM,
                                                       cgOsm.m_numInRegisterInt, cgOsm.m_numInRegisterFloat, false)
                                ->Fixup(curCodePos, &cgOsm.m_fixupData);

                        cgOsm.ConsumeInts(1);
                        assert(cgOsm.m_numInRegisterInt == 0);
                        assert(cgOsm.m_numInRegisterFloat == 0);
                        g_wasmCommonOpcodeManager->Get(WasmOpcode::XX_SWITCH_SF, 0, 0, false)->Fixup(curCodePos, &cgOsm.m_fixupData);
                        uint8_t* fnAddr = g_wasmCallManager.EmitCall(curCodePos);
                        UnalignedAddAndWriteback<uint32_t>(fnAddr, static_cast<uint32_t>(reinterpret_cast<uint64_t>(codeRegionBegin + 16)));
                        uint8_t* fnStackSizeAddr2 = g_wasmCallManager.EmitCleanup(curCodePos, WasmValueType::I32, curOpcode->m_spillOutput, &cgOsm.m_fixupData);
                        UnalignedWrite<uint32_t>(fnStackSizeAddr2, 40);
                        cgOsm.ProduceOutput(WasmValueType::I32, curOpcode->m_spillOutput);
                        curOperand++;
                    }
                    else
                    {
                        assert(false && "unhandled opcode");
                    }
                }
                curOpcode++;
            }
            assert(curOpcode == opcodeEnd && curOperand == operandEnd);
            assert(cgBlockStack.empty());
            assert(curBrOffsetListPos == brOffsetPatchArray);
            assert(curCodePos <= m_cgMan.m_materializedLimit);
        }
        // printf("%d\n", static_cast<int>(operandList.size()));
        assert(!reader.HasMore());

        {
            TempArenaAllocator taa;
            uint32_t numFuncTypes = m_functionTypeIndices.GetNumFunctionTypes();
            uint32_t* sz = new (taa) uint32_t[numFuncTypes];
            for (uint32_t i = 0; i < numFuncTypes; i++)
            {
                sz[i] = 40;
            }
            for (uint32_t curFunc = 0; curFunc < m_functionDeclarations.m_numFunctions; curFunc++)
            {
                uint32_t funcTypeIdx = m_functionDeclarations.m_functionDeclarations[curFunc];
                assert(funcTypeIdx < numFuncTypes);
                sz[funcTypeIdx] = std::max(sz[funcTypeIdx], m_functionDeclarations.m_functionStackSize[curFunc]);
            }

            for (auto p : indirectCallStackSizeFixups)
            {
                assert(p.second < numFuncTypes);
                uint32_t v = sz[p.second];
                UnalignedWrite<uint32_t>(p.first, v);
            }
        }

        for (auto p : callStackSizeFixups)
        {
            uint32_t v = m_functionDeclarations.m_functionStackSize[p.second];
            UnalignedWrite<uint32_t>(p.first, v);
        }

        for (auto p : callFuncAddressFixups)
        {
            uint32_t v = static_cast<uint32_t>(reinterpret_cast<uint64_t>(m_functionDeclarations.m_functionEntryPoint[p.second]));
            UnalignedAddAndWriteback<uint32_t>(p.first, v);
        }

        for (uint32_t ord = 0; ord < m_exportSection.m_numFunctionsExported; ord++)
        {
            m_cgMan.Reserve(256);
            uint32_t funcIdx = m_exportSection.m_exportedFunctions[ord].m_entityIdx;
            m_exportSection.m_exportedFunctionAddresses[ord] = curCodePos;
            uint32_t funcTypeIdx = m_functionDeclarations.m_functionDeclarations[funcIdx];
            WasmFunctionType funcType = m_functionTypeIndices.GetFunctionTypeFromIdx(funcTypeIdx);
            WasmValueType returnType = WasmValueType::X_END_OF_ENUM;
            if (funcType.m_numReturns > 0)
            {
                assert(funcType.m_numReturns == 1);
                returnType = funcType.GetReturnType(0);
            }
            g_wasmCppEntryManager.Codegen(curCodePos, returnType, m_functionDeclarations.m_functionEntryPoint[funcIdx]);
        }

        // printf("code region start: 0x%llx\n", reinterpret_cast<unsigned long long>(codeRegionBegin));
        // printf("generated code length: %d\n", static_cast<int>(curCodePos - codeRegionBegin));

        uint64_t negPartLen = brTableBaseOffset + m_codegenAuxilaryDataTable.size() * 8;
        m_runtimeMemory = WasmRuntimeMemory::Create(negPartLen, 0 /*numInitialPages*/);

        // populate global data
        //
        {
            if (m_globalSection.m_numImportedGlobals > 0)
            {
                DEBUG_ONLY(printf("[ERROR] Imported globals is currently unsupported. "
                       "Codegen will continue, but the generated code will not be runnable.\n");)
            }
            uint64_t* tb = reinterpret_cast<uint64_t*>(m_runtimeMemory->GetMemZero() - 16 - m_globalSection.m_numGlobals * 8);
            memset(tb, 0, m_globalSection.m_numGlobals * 8);
            tb = reinterpret_cast<uint64_t*>(m_runtimeMemory->GetMemZero() - 16);
            for (uint32_t ord = m_globalSection.m_numImportedGlobals; ord < m_globalSection.m_numGlobals; ord++)
            {
                WasmConstantExpression& initExpr = m_globalSection.m_initExprs[ord - m_globalSection.m_numImportedGlobals];
                if (initExpr.m_isInitByGlobal)
                {
                    DEBUG_ONLY(printf("[ERROR] global initialized by another global is currently unsupported. "
                           "Codegen will continue, but the generated code will not be runnable.\n");)
                }
                tb--;
                memcpy(tb, initExpr.m_initRawBytes, 8);
            }
        }

        // populate call_indirect data
        //
        {
            uint64_t* tb = reinterpret_cast<uint64_t*>(m_runtimeMemory->GetMemZero() - brTableBaseOffset);
            uint32_t tableSize = m_tableSection.m_limit.m_minSize;
            memset(tb, 255, sizeof(uint64_t) * 2 * tableSize);
            for (uint32_t ord = 0; ord < m_elementSection.m_numRecords; ord++)
            {
                WasmElementRecord& r = m_elementSection.m_records[ord];
                if (r.m_offset.m_isInitByGlobal)
                {
                    DEBUG_ONLY(printf("[ERROR] element section initialized by global is currently unsupported. "
                           "Codegen will continue, but the generated code will not be runnable.\n");)
                }
                else
                {
                    uint32_t offset;
                    memcpy(&offset, r.m_offset.m_initRawBytes, 4);
                    assert(offset + r.m_length <= tableSize);
                    uint64_t* fillSlot = tb + offset * 2;
                    for (uint32_t i = 0; i < r.m_length; i++)
                    {
                        uint32_t funcIdx = r.m_contents[i];
                        uint32_t funcType = m_functionDeclarations.GetFunctionTypeIdxFromFunctionIdx(funcIdx);
                        fillSlot[0] = funcType;
                        fillSlot[1] = reinterpret_cast<uint64_t>(m_functionDeclarations.m_functionEntryPoint[funcIdx]);
                        fillSlot += 2;
                    }
                }
            }
        }

        // populate br_table data
        //
        {
            uint64_t* dst = reinterpret_cast<uint64_t*>(m_runtimeMemory->GetMemZero() - brTableBaseOffset - 8);
            for (uint64_t v : m_codegenAuxilaryDataTable)
            {
                *dst = v;
                dst--;
            }
        }
        m_runtimeMemory->SetGs();
    }

    TempArenaAllocator m_alloc;
    MemoryMappedFile m_file;
    WasmFunctionTypeSection m_functionTypeIndices;
    WasmImportSection m_importSection;
    WasmFunctionDeclarationSection m_functionDeclarations;
    WasmTableSection m_tableSection;
    WasmMemorySection m_memorySection;
    WasmGlobalSection m_globalSection;
    WasmExportSection m_exportSection;
    WasmStartSection m_startSection;
    WasmElementSection m_elementSection;
    WasmDataSection m_dataSection;
    std::vector<uint64_t> m_codegenAuxilaryDataTable;
    WasmRuntimeMemory* m_runtimeMemory;
    WasmGeneratedCodeManager m_cgMan;
#ifndef NDEBUG
    bool m_initialized;
#endif
};

}   // namespace PochiVM

double ComputeAverage(const std::function<double()>& benchmarkFn)
{
    int numRuns = 5;
    double sum = 0;
    for (int i = 0; i < numRuns; i++)
    {
        double result = benchmarkFn();
        sum += result;
    }
    return sum / numRuns;
}

double CompileWasiModule(const std::string& s)
{
    using namespace PochiVM;

    AutoThreadErrorContext atec;

    double totalCompilationTime;
    WasmModule mod;
    ReleaseAssert(mod.ParseModule(s.c_str(), &totalCompilationTime));

    return totalCompilationTime;
}

void RunWasiModule(const std::string& s)
{
    using namespace PochiVM;

    AutoThreadErrorContext atec;

    WasmModule mod;
    ReleaseAssert(mod.ParseModule(s.c_str()));

    ReleaseAssert(mod.m_exportSection.m_numFunctionsExported == 1);
    uint32_t funcIdx = mod.m_exportSection.m_exportedFunctions[0].m_entityIdx;
    uint32_t funcTypeIdx = mod.m_functionDeclarations.m_functionDeclarations[funcIdx];
    WasmFunctionType funcType = mod.m_functionTypeIndices.GetFunctionTypeFromIdx(funcTypeIdx);
    ReleaseAssert(funcType.m_numParams == 0 && funcType.m_numReturns == 0);
    uint8_t* p = mod.m_exportSection.m_exportedFunctionAddresses[0];
    uint32_t sz = mod.m_functionDeclarations.m_functionStackSize[funcIdx];
    void* buf = alloca(sz);
    memset(buf, 0, sz);
    reinterpret_cast<void(*)(uintptr_t)>(p)(reinterpret_cast<uintptr_t>(buf));
}

TEST(WasmCompilation, BenchmarkAll)
{
    printf("Coremark Compilation time: %.7lf\n", ComputeAverage([]() {
        return CompileWasiModule("wasm_inputs/coremark-wasi.wasm");
    }));

    constexpr int totalBenchmarks = 30;
    double total = 0;
    const char* const files[totalBenchmarks] = {
        "2mm",
        "3mm",
        "adi",
        "atax",
        "bicg",
        "cholesky",
        "correlation",
        "covariance",
        "deriche",
        "doitgen",
        "durbin",
        "fdtd_2d",
        "floyd_warshall",
        "gemm",
        "gemver",
        "gesummv",
        "gramschmidt",
        "heat_3d",
        "jacobi_1d",
        "jacobi_2d",
        "ludcmp",
        "lu",
        "mvt",
        "nussinov",
        "seidel_2d",
        "symm",
        "syr2k",
        "syrk",
        "trisolv",
        "trmm"
    };
    for (int i = 0; i < totalBenchmarks; i++)
    {
        total += ComputeAverage([&]() {
            return CompileWasiModule(std::string("wasm_inputs/PolyBenchC/") + files[i] + ".wasm");
        });
    }
    printf("PolyBench Total Compilation time: %.7lf\n", total);

    printf("AutoCAD Compilation time: %.7lf\n", ComputeAverage([]() {
        return CompileWasiModule("wasm_inputs/autocad.wasm");
    }));

    printf("Clang.wasm Compilation time: %.7lf\n", ComputeAverage([]() {
        return CompileWasiModule("wasm_inputs/clang.wasm");
    }));
}

TEST(WasmExecution, CoremarkWasiBenchmark)
{
    fflush(stdout);
    printf("*** Benchmark Coremark (Run 1) ***\n");
    fflush(stdout);
    RunWasiModule("wasm_inputs/coremark-wasi.wasm");
    fflush(stdout);
    printf("*** Benchmark Coremark (Run 2) ***\n");
    fflush(stdout);
    RunWasiModule("wasm_inputs/coremark-wasi.wasm");
    fflush(stdout);
    printf("*** Benchmark Coremark (Run 3) ***\n");
    fflush(stdout);
    RunWasiModule("wasm_inputs/coremark-wasi.wasm");
}

#define STRINGIFY(x) #x
#define TOSTRING(x) STRINGIFY(x)

#define GENERATE_POLYBENCHC_TEST(name)                                  \
TEST(WasmExecution, PolyBenchC_ ## name)                                \
{                                                                       \
    std::string s = "wasm_inputs/PolyBenchC/" TOSTRING(name) ".wasm";   \
    fflush(stdout);                                                     \
    printf("*** Benchmark PolyBench." TOSTRING(name) " (Run 1) ***\n"); \
    fflush(stdout);                                                     \
    RunWasiModule(s);                                                   \
    fflush(stdout);                                                     \
    printf("*** Benchmark PolyBench." TOSTRING(name) " (Run 2) ***\n"); \
    fflush(stdout);                                                     \
    RunWasiModule(s);                                                   \
    fflush(stdout);                                                     \
    printf("*** Benchmark PolyBench." TOSTRING(name) " (Run 3) ***\n"); \
    fflush(stdout);                                                     \
    RunWasiModule(s);                                                   \
}

GENERATE_POLYBENCHC_TEST(2mm)
GENERATE_POLYBENCHC_TEST(3mm)
GENERATE_POLYBENCHC_TEST(adi)
GENERATE_POLYBENCHC_TEST(atax)
GENERATE_POLYBENCHC_TEST(bicg)
GENERATE_POLYBENCHC_TEST(cholesky)
GENERATE_POLYBENCHC_TEST(correlation)
GENERATE_POLYBENCHC_TEST(covariance)
GENERATE_POLYBENCHC_TEST(deriche)
GENERATE_POLYBENCHC_TEST(doitgen)
GENERATE_POLYBENCHC_TEST(durbin)
GENERATE_POLYBENCHC_TEST(fdtd_2d)
GENERATE_POLYBENCHC_TEST(floyd_warshall)
GENERATE_POLYBENCHC_TEST(gemm)
GENERATE_POLYBENCHC_TEST(gemver)
GENERATE_POLYBENCHC_TEST(gesummv)
GENERATE_POLYBENCHC_TEST(gramschmidt)
GENERATE_POLYBENCHC_TEST(heat_3d)
GENERATE_POLYBENCHC_TEST(jacobi_1d)
GENERATE_POLYBENCHC_TEST(jacobi_2d)
GENERATE_POLYBENCHC_TEST(ludcmp)
GENERATE_POLYBENCHC_TEST(lu)
GENERATE_POLYBENCHC_TEST(mvt)
GENERATE_POLYBENCHC_TEST(nussinov)
GENERATE_POLYBENCHC_TEST(seidel_2d)
GENERATE_POLYBENCHC_TEST(symm)
GENERATE_POLYBENCHC_TEST(syr2k)
GENERATE_POLYBENCHC_TEST(syrk)
GENERATE_POLYBENCHC_TEST(trisolv)
GENERATE_POLYBENCHC_TEST(trmm)

#undef GENERATE_POLYBENCHC_TEST
#undef TOSTRING
#undef STRINGIFY
