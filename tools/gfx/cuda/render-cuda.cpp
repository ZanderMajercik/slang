#include "render-cuda.h"

#ifdef GFX_ENABLE_CUDA
#include <cuda.h>
#include <cuda_runtime_api.h>
#include "core/slang-basic.h"
#include "core/slang-blob.h"
#include "core/slang-std-writers.h"

#include "slang.h"
#include "slang-com-ptr.h"
#include "slang-com-helper.h"
#include "../command-writer.h"
#include "../renderer-shared.h"
#include "../mutable-shader-object.h"
#include "../simple-transient-resource-heap.h"
#include "../slang-context.h"

#   ifdef RENDER_TEST_OPTIX

// The `optix_stubs.h` header produces warnings when compiled with MSVC
#       ifdef _MSC_VER
#           pragma warning(disable: 4996)
#       endif

#       include <optix.h>
#       include <optix_function_table_definition.h>
#       include <optix_stubs.h>
#   endif

#endif

namespace gfx
{
#ifdef GFX_ENABLE_CUDA
using namespace Slang;

SLANG_FORCE_INLINE static bool _isError(CUresult result) { return result != 0; }
SLANG_FORCE_INLINE static bool _isError(cudaError_t result) { return result != 0; }

// A enum used to control if errors are reported on failure of CUDA call.
enum class CUDAReportStyle
{
    Normal,
    Silent,
};

struct CUDAErrorInfo
{
    CUDAErrorInfo(
        const char* filePath,
        int lineNo,
        const char* errorName = nullptr,
        const char* errorString = nullptr)
        : m_filePath(filePath)
        , m_lineNo(lineNo)
        , m_errorName(errorName)
        , m_errorString(errorString)
    {}
    SlangResult handle() const
    {
        StringBuilder builder;
        builder << "Error: " << m_filePath << " (" << m_lineNo << ") :";

        if (m_errorName)
        {
            builder << m_errorName << " : ";
        }
        if (m_errorString)
        {
            builder << m_errorString;
        }

        getDebugCallback()->handleMessage(DebugMessageType::Error, DebugMessageSource::Driver,
            builder.getUnownedSlice().begin());

        // Slang::signalUnexpectedError(builder.getBuffer());
        return SLANG_FAIL;
    }

    const char* m_filePath;
    int m_lineNo;
    const char* m_errorName;
    const char* m_errorString;
};

// If this code path is enabled, CUDA errors will be reported directly to StdWriter::out stream.

static SlangResult _handleCUDAError(CUresult cuResult, const char* file, int line)
{
    CUDAErrorInfo info(file, line);
    cuGetErrorString(cuResult, &info.m_errorString);
    cuGetErrorName(cuResult, &info.m_errorName);
    return info.handle();
}

static SlangResult _handleCUDAError(cudaError_t error, const char* file, int line)
{
    return CUDAErrorInfo(file, line, cudaGetErrorName(error), cudaGetErrorString(error)).handle();
}

#    define SLANG_CUDA_HANDLE_ERROR(x) _handleCUDAError(x, __FILE__, __LINE__)

#    define SLANG_CUDA_RETURN_ON_FAIL(x)              \
        {                                             \
            auto _res = x;                            \
            if (_isError(_res))                       \
                return SLANG_CUDA_HANDLE_ERROR(_res); \
        }
#    define SLANG_CUDA_RETURN_WITH_REPORT_ON_FAIL(x, r)                               \
        {                                                                             \
            auto _res = x;                                                            \
            if (_isError(_res))                                                       \
            {                                                                         \
                return (r == CUDAReportStyle::Normal) ? SLANG_CUDA_HANDLE_ERROR(_res) \
                                                      : SLANG_FAIL;                   \
            }                                                                         \
        }

#    define SLANG_CUDA_ASSERT_ON_FAIL(x)           \
        {                                          \
            auto _res = x;                         \
            if (_isError(_res))                    \
            {                                      \
                SLANG_ASSERT(!"Failed CUDA call"); \
            };                                     \
        }

#    ifdef RENDER_TEST_OPTIX

static bool _isError(OptixResult result) { return result != OPTIX_SUCCESS; }

#        if 1
static SlangResult _handleOptixError(OptixResult result, char const* file, int line)
{
    fprintf(
        stderr,
        "%s(%d): optix: %s (%s)\n",
        file,
        line,
        optixGetErrorString(result),
        optixGetErrorName(result));
    return SLANG_FAIL;
}
#            define SLANG_OPTIX_HANDLE_ERROR(RESULT) _handleOptixError(RESULT, __FILE__, __LINE__)
#        else
#            define SLANG_OPTIX_HANDLE_ERROR(RESULT) SLANG_FAIL
#        endif

#        define SLANG_OPTIX_RETURN_ON_FAIL(EXPR)           \
            do                                             \
            {                                              \
                auto _res = EXPR;                          \
                if (_isError(_res))                        \
                    return SLANG_OPTIX_HANDLE_ERROR(_res); \
            } while (0)

void _optixLogCallback(unsigned int level, const char* tag, const char* message, void* userData)
{
    fprintf(stderr, "optix: %s (%s)\n", message, tag);
}

#    endif

class CUDAContext : public RefObject
{
public:
    CUcontext m_context = nullptr;
    ~CUDAContext() { cuCtxDestroy(m_context); }
};

class MemoryCUDAResource : public BufferResource
{
public:
    MemoryCUDAResource(const Desc& _desc)
        : BufferResource(_desc)
    {}

    ~MemoryCUDAResource()
    {
        if (m_cudaMemory)
        {
            SLANG_CUDA_ASSERT_ON_FAIL(cudaFree(m_cudaMemory));
        }
    }

    uint64_t getBindlessHandle() { return (uint64_t)m_cudaMemory; }

    void* m_cudaExternalMemory = nullptr;
    void* m_cudaMemory = nullptr;

    RefPtr<CUDAContext> m_cudaContext;

    virtual SLANG_NO_THROW DeviceAddress SLANG_MCALL getDeviceAddress() override
    {
        return (DeviceAddress)m_cudaMemory;
    }

    virtual SLANG_NO_THROW Result SLANG_MCALL getNativeResourceHandle(InteropHandle* outHandle) override
    {
        outHandle->handleValue = getBindlessHandle();
        outHandle->api = InteropHandleAPI::CUDA;
        return SLANG_OK;
    }
};

class TextureCUDAResource : public TextureResource
{
public:
    TextureCUDAResource(const TextureResource::Desc& desc)
        : TextureResource(desc)
    {}
    ~TextureCUDAResource()
    {
        if (m_cudaSurfObj)
        {
            SLANG_CUDA_ASSERT_ON_FAIL(cuSurfObjectDestroy(m_cudaSurfObj));
        }
        if (m_cudaTexObj)
        {
            SLANG_CUDA_ASSERT_ON_FAIL(cuTexObjectDestroy(m_cudaTexObj));
        }
        if (m_cudaArray)
        {
            SLANG_CUDA_ASSERT_ON_FAIL(cuArrayDestroy(m_cudaArray));
        }
        if (m_cudaMipMappedArray)
        {
            SLANG_CUDA_ASSERT_ON_FAIL(cuMipmappedArrayDestroy(m_cudaMipMappedArray));
        }
    }

    uint64_t getBindlessHandle() { return (uint64_t)m_cudaTexObj; }

    // The texObject is for reading 'texture' like things. This is an opaque type, that's backed by
    // a long long
    CUtexObject m_cudaTexObj = CUtexObject();

    // The surfObj is for reading/writing 'texture like' things, but not for sampling.
    CUsurfObject m_cudaSurfObj = CUsurfObject();

    CUarray m_cudaArray = CUarray();
    CUmipmappedArray m_cudaMipMappedArray = CUmipmappedArray();

    void* m_cudaExternalMemory = nullptr;

    RefPtr<CUDAContext> m_cudaContext;

    virtual SLANG_NO_THROW Result SLANG_MCALL getNativeResourceHandle(InteropHandle* outHandle) override
    {
        outHandle->handleValue = getBindlessHandle();
        outHandle->api = InteropHandleAPI::CUDA;
        return SLANG_OK;
    }
};

class CUDAResourceView : public ResourceViewBase
{
public:
    RefPtr<MemoryCUDAResource> memoryResource = nullptr;
    RefPtr<TextureCUDAResource> textureResource = nullptr;
    void* proxyBuffer = nullptr;
};

class CUDAShaderObjectLayout : public ShaderObjectLayoutBase
{
public:
    struct BindingRangeInfo
    {
        slang::BindingType bindingType;
        Index count;
        Index baseIndex; // Flat index for sub-ojects
        Index subObjectIndex;

        // TODO: The `uniformOffset` field should be removed,
        // since it cannot be supported by the Slang reflection
        // API once we fix some design issues.
        //
        // It is only being used today for pre-allocation of sub-objects
        // for constant buffers and parameter blocks (which should be
        // deprecated/removed anyway).
        //
        // Note: We would need to bring this field back, plus
        // a lot of other complexity, if we ever want to support
        // setting of resources/buffers directly by a binding
        // range index and array index.
        //
        Index uniformOffset; // Uniform offset for a resource typed field.
    };

    struct SubObjectRangeInfo
    {
        RefPtr<CUDAShaderObjectLayout> layout;
        Index bindingRangeIndex;
    };

    List<SubObjectRangeInfo> subObjectRanges;
    List<BindingRangeInfo> m_bindingRanges;

    Index m_subObjectCount = 0;
    Index m_resourceCount = 0;

    CUDAShaderObjectLayout(RendererBase* renderer, slang::TypeLayoutReflection* layout)
    {
        m_elementTypeLayout = _unwrapParameterGroups(layout, m_containerType);

        initBase(renderer, m_elementTypeLayout);

        // Compute the binding ranges that are used to store
        // the logical contents of the object in memory. These will relate
        // to the descriptor ranges in the various sets, but not always
        // in a one-to-one fashion.

        SlangInt bindingRangeCount = m_elementTypeLayout->getBindingRangeCount();
        for (SlangInt r = 0; r < bindingRangeCount; ++r)
        {
            slang::BindingType slangBindingType = m_elementTypeLayout->getBindingRangeType(r);
            SlangInt count = m_elementTypeLayout->getBindingRangeBindingCount(r);
            slang::TypeLayoutReflection* slangLeafTypeLayout =
                m_elementTypeLayout->getBindingRangeLeafTypeLayout(r);

            SlangInt descriptorSetIndex = m_elementTypeLayout->getBindingRangeDescriptorSetIndex(r);
            SlangInt rangeIndexInDescriptorSet =
                m_elementTypeLayout->getBindingRangeFirstDescriptorRangeIndex(r);

            // TODO: This logic assumes that for any binding range that might consume
            // multiple kinds of resources, the descriptor range for its uniform
            // usage will be the first one in the range.
            //
            // We need to decide whether that assumption is one we intend to support
            // applications making, or whether they should be forced to perform a
            // linear search over the descriptor ranges for a specific binding range.
            //
            auto uniformOffset = m_elementTypeLayout->getDescriptorSetDescriptorRangeIndexOffset(
                descriptorSetIndex, rangeIndexInDescriptorSet);

            Index baseIndex = 0;
            Index subObjectIndex = 0;
            switch (slangBindingType)
            {
            case slang::BindingType::ConstantBuffer:
            case slang::BindingType::ParameterBlock:
            case slang::BindingType::ExistentialValue:
                baseIndex = m_subObjectCount;
                subObjectIndex = baseIndex;
                m_subObjectCount += count;
                break;
            case slang::BindingType::RawBuffer:
            case slang::BindingType::MutableRawBuffer:
                if (slangLeafTypeLayout->getType()->getElementType() != nullptr)
                {
                    // A structured buffer occupies both a resource slot and
                    // a sub-object slot.
                    subObjectIndex = m_subObjectCount;
                    m_subObjectCount += count;
                }
                baseIndex = m_resourceCount;
                m_resourceCount += count;
                break;
            default:
                baseIndex = m_resourceCount;
                m_resourceCount += count;
                break;
            }

            BindingRangeInfo bindingRangeInfo;
            bindingRangeInfo.bindingType = slangBindingType;
            bindingRangeInfo.count = count;
            bindingRangeInfo.baseIndex = baseIndex;
            bindingRangeInfo.uniformOffset = uniformOffset;
            bindingRangeInfo.subObjectIndex = subObjectIndex;
            m_bindingRanges.add(bindingRangeInfo);
        }

        SlangInt subObjectRangeCount = m_elementTypeLayout->getSubObjectRangeCount();
        for (SlangInt r = 0; r < subObjectRangeCount; ++r)
        {
            SlangInt bindingRangeIndex = m_elementTypeLayout->getSubObjectRangeBindingRangeIndex(r);
            auto slangBindingType = m_elementTypeLayout->getBindingRangeType(bindingRangeIndex);
            slang::TypeLayoutReflection* slangLeafTypeLayout =
                m_elementTypeLayout->getBindingRangeLeafTypeLayout(bindingRangeIndex);

            // A sub-object range can either represent a sub-object of a known
            // type, like a `ConstantBuffer<Foo>` or `ParameterBlock<Foo>`
            // (in which case we can pre-compute a layout to use, based on
            // the type `Foo`) *or* it can represent a sub-object of some
            // existential type (e.g., `IBar`) in which case we cannot
            // know the appropraite type/layout of sub-object to allocate.
            //
            RefPtr<CUDAShaderObjectLayout> subObjectLayout;
            if (slangBindingType != slang::BindingType::ExistentialValue)
            {
                subObjectLayout =
                    new CUDAShaderObjectLayout(renderer, slangLeafTypeLayout->getElementTypeLayout());
            }

            SubObjectRangeInfo subObjectRange;
            subObjectRange.bindingRangeIndex = bindingRangeIndex;
            subObjectRange.layout = subObjectLayout;
            subObjectRanges.add(subObjectRange);
        }
    }

    Index getResourceCount() const { return m_resourceCount; }
    Index getSubObjectCount() const { return m_subObjectCount; }
    List<SubObjectRangeInfo>& getSubObjectRanges() { return subObjectRanges; }
    BindingRangeInfo getBindingRange(Index index) { return m_bindingRanges[index]; }
    Index getBindingRangeCount() const { return m_bindingRanges.getCount(); }
};

class CUDAProgramLayout : public CUDAShaderObjectLayout
{
public:
    slang::ProgramLayout* programLayout = nullptr;
    List<RefPtr<CUDAShaderObjectLayout>> entryPointLayouts;
    CUDAProgramLayout(RendererBase* renderer, slang::ProgramLayout* inProgramLayout)
        : CUDAShaderObjectLayout(renderer, inProgramLayout->getGlobalParamsTypeLayout())
        , programLayout(inProgramLayout)
    {
        for (UInt i =0; i< programLayout->getEntryPointCount(); i++)
        {
            entryPointLayouts.add(new CUDAShaderObjectLayout(
                renderer,
                programLayout->getEntryPointByIndex(i)->getTypeLayout()));
        }

    }

    int getKernelIndex(UnownedStringSlice kernelName)
    {
        for (int i = 0; i < (int)programLayout->getEntryPointCount(); i++)
        {
            auto entryPoint = programLayout->getEntryPointByIndex(i);
            if (kernelName == entryPoint->getName())
            {
                return i;
            }
        }
        return -1;
    }

    void getKernelThreadGroupSize(int kernelIndex, UInt* threadGroupSizes)
    {
        auto entryPoint = programLayout->getEntryPointByIndex(kernelIndex);
        entryPoint->getComputeThreadGroupSize(3, threadGroupSizes);
    }
};

class CUDAShaderObjectData
{
public:
    bool isHostOnly = false;
    Slang::RefPtr<MemoryCUDAResource> m_bufferResource;
    Slang::RefPtr<CUDAResourceView> m_bufferView;
    Slang::List<uint8_t> m_cpuBuffer;
    Result setCount(Index count)
    {
        if (isHostOnly)
        {
            m_cpuBuffer.setCount(count);
            if (!m_bufferView)
            {
                IResourceView::Desc viewDesc = {};
                viewDesc.type = IResourceView::Type::UnorderedAccess;
                m_bufferView = new CUDAResourceView();
                m_bufferView->proxyBuffer = m_cpuBuffer.getBuffer();
                m_bufferView->m_desc = viewDesc;
            }
            return SLANG_OK;
        }

        if (!m_bufferResource)
        {
            IBufferResource::Desc desc;
            desc.type = IResource::Type::Buffer;
            desc.sizeInBytes = count;
            m_bufferResource = new MemoryCUDAResource(desc);
            if (count)
            {
                SLANG_CUDA_RETURN_ON_FAIL(cudaMalloc(&m_bufferResource->m_cudaMemory, (size_t)count));
            }
            IResourceView::Desc viewDesc = {};
            viewDesc.type = IResourceView::Type::UnorderedAccess;
            m_bufferView = new CUDAResourceView();
            m_bufferView->memoryResource = m_bufferResource;
            m_bufferView->m_desc = viewDesc;
        }
        auto oldSize = m_bufferResource->getDesc()->sizeInBytes;
        if ((size_t)count != oldSize)
        {
            void* newMemory = nullptr;
            if (count)
            {
                SLANG_CUDA_RETURN_ON_FAIL(cudaMalloc(&newMemory, (size_t)count));
            }
            if (oldSize)
            {
                SLANG_CUDA_RETURN_ON_FAIL(cudaMemcpy(
                    newMemory,
                    m_bufferResource->m_cudaMemory,
                    Math::Min((size_t)count, oldSize),
                    cudaMemcpyDefault));
            }
            cudaFree(m_bufferResource->m_cudaMemory);
            m_bufferResource->m_cudaMemory = newMemory;
            m_bufferResource->getDesc()->sizeInBytes = count;
        }
        return SLANG_OK;
    }

    Slang::Index getCount()
    {
        if (isHostOnly)
            return m_cpuBuffer.getCount();
        if (m_bufferResource)
            return (Slang::Index)(m_bufferResource->getDesc()->sizeInBytes);
        else
            return 0;
    }

    void* getBuffer()
    {
        if (isHostOnly)
            return m_cpuBuffer.getBuffer();

        if (m_bufferResource)
            return m_bufferResource->m_cudaMemory;
        return nullptr;
    }

    /// Returns a resource view for GPU access into the buffer content.
    ResourceViewBase* getResourceView(
        RendererBase* device,
        slang::TypeLayoutReflection* elementLayout,
        slang::BindingType bindingType)
    {
        SLANG_UNUSED(device);
        m_bufferResource->getDesc()->elementSize = (int)elementLayout->getSize();
        return m_bufferView.Ptr();
    }
};

class CUDAShaderObject
    : public ShaderObjectBaseImpl<CUDAShaderObject, CUDAShaderObjectLayout, CUDAShaderObjectData>
{
    typedef ShaderObjectBaseImpl<CUDAShaderObject, CUDAShaderObjectLayout, CUDAShaderObjectData>
        Super;

public:
    List<RefPtr<CUDAResourceView>> resources;

    virtual SLANG_NO_THROW Result SLANG_MCALL
        init(IDevice* device, CUDAShaderObjectLayout* typeLayout);

    virtual SLANG_NO_THROW UInt SLANG_MCALL getEntryPointCount() override { return 0; }
    virtual SLANG_NO_THROW Result SLANG_MCALL
        getEntryPoint(UInt index, IShaderObject** outEntryPoint) override
    {
        *outEntryPoint = nullptr;
        return SLANG_OK;
    }

    virtual SLANG_NO_THROW const void* SLANG_MCALL getRawData() override
    {
        return m_data.getBuffer();
    }

    virtual SLANG_NO_THROW size_t SLANG_MCALL getSize() override
    {
        return (size_t)m_data.getCount();
    }

    virtual SLANG_NO_THROW Result SLANG_MCALL
        setData(ShaderOffset const& offset, void const* data, size_t size) override
    {
        size = Math::Min(size, (size_t)m_data.getCount() - offset.uniformOffset);
        SLANG_CUDA_RETURN_ON_FAIL(cudaMemcpy(
            (uint8_t*)m_data.getBuffer() + offset.uniformOffset, data, size, cudaMemcpyDefault));
        return SLANG_OK;
    }
    virtual SLANG_NO_THROW Result SLANG_MCALL
        setResource(ShaderOffset const& offset, IResourceView* resourceView) override
    {
        if (!resourceView)
            return SLANG_OK;

        auto layout = getLayout();

        auto bindingRangeIndex = offset.bindingRangeIndex;
        SLANG_ASSERT(bindingRangeIndex >= 0);
        SLANG_ASSERT(bindingRangeIndex < layout->m_bindingRanges.getCount());

        auto& bindingRange = layout->m_bindingRanges[bindingRangeIndex];

        auto viewIndex = bindingRange.baseIndex + offset.bindingArrayIndex;
        auto cudaView = static_cast<CUDAResourceView*>(resourceView);

        resources[viewIndex] = cudaView;

        if (cudaView->textureResource)
        {
            if (cudaView->m_desc.type == IResourceView::Type::UnorderedAccess)
            {
                auto handle = cudaView->textureResource->m_cudaSurfObj;
                setData(offset, &handle, sizeof(uint64_t));
            }
            else
            {
                auto handle = cudaView->textureResource->getBindlessHandle();
                setData(offset, &handle, sizeof(uint64_t));
            }
        }
        else if (cudaView->memoryResource)
        {
            auto handle = cudaView->memoryResource->getBindlessHandle();
            setData(offset, &handle, sizeof(handle));
            auto sizeOffset = offset;
            sizeOffset.uniformOffset += sizeof(handle);
            auto& desc = *cudaView->memoryResource->getDesc();
            size_t size = desc.sizeInBytes;
            if (desc.elementSize > 1)
                size /= desc.elementSize;
            setData(sizeOffset, &size, sizeof(size));
        }
        else if (cudaView->proxyBuffer)
        {
            auto handle = cudaView->proxyBuffer;
            setData(offset, &handle, sizeof(handle));
            auto sizeOffset = offset;
            sizeOffset.uniformOffset += sizeof(handle);
            auto& desc = *cudaView->memoryResource->getDesc();
            size_t size = desc.sizeInBytes;
            if (desc.elementSize > 1)
                size /= desc.elementSize;
            setData(sizeOffset, &size, sizeof(size));
        }
        return SLANG_OK;
    }
    virtual SLANG_NO_THROW Result SLANG_MCALL
        setObject(ShaderOffset const& offset, IShaderObject* object) override
    {
        SLANG_RETURN_ON_FAIL(Super::setObject(offset, object));

        auto bindingRangeIndex = offset.bindingRangeIndex;
        auto& bindingRange = getLayout()->m_bindingRanges[bindingRangeIndex];

        CUDAShaderObject* subObject = static_cast<CUDAShaderObject*>(object);
        switch (bindingRange.bindingType)
        {
        default:
            {
                void* subObjectDataBuffer = subObject->getBuffer();
                SLANG_RETURN_ON_FAIL(setData(offset, &subObjectDataBuffer, sizeof(void*)));
            }
            break;
        case slang::BindingType::ExistentialValue:
        case slang::BindingType::RawBuffer:
        case slang::BindingType::MutableRawBuffer:
            break;
        }
        return SLANG_OK;
    }
    virtual SLANG_NO_THROW Result SLANG_MCALL
        setSampler(ShaderOffset const& offset, ISamplerState* sampler) override
    {
        SLANG_UNUSED(sampler);
        SLANG_UNUSED(offset);
        return SLANG_OK;
    }
    virtual SLANG_NO_THROW Result SLANG_MCALL setCombinedTextureSampler(
        ShaderOffset const& offset, IResourceView* textureView, ISamplerState* sampler) override
    {
        SLANG_UNUSED(sampler);
        setResource(offset, textureView);
        return SLANG_OK;
    }
};

class CUDAMutableShaderObject : public MutableShaderObject< CUDAMutableShaderObject, CUDAShaderObjectLayout>
{};

class CUDAEntryPointShaderObject : public CUDAShaderObject
{
public:
    CUDAEntryPointShaderObject() { m_data.isHostOnly = true; }
};

class CUDARootShaderObject : public CUDAShaderObject
{
public:
    virtual SLANG_NO_THROW uint32_t SLANG_MCALL addRef() override { return 1; }
    virtual SLANG_NO_THROW uint32_t SLANG_MCALL release() override { return 1; }
public:
    List<RefPtr<CUDAEntryPointShaderObject>> entryPointObjects;
    virtual SLANG_NO_THROW Result SLANG_MCALL
        init(IDevice* device, CUDAShaderObjectLayout* typeLayout) override;
    virtual SLANG_NO_THROW UInt SLANG_MCALL getEntryPointCount() override { return entryPointObjects.getCount(); }
    virtual SLANG_NO_THROW Result SLANG_MCALL
        getEntryPoint(UInt index, IShaderObject** outEntryPoint) override
    {
        returnComPtr(outEntryPoint, entryPointObjects[index]);
        return SLANG_OK;
    }
    virtual Result collectSpecializationArgs(ExtendedShaderObjectTypeList& args) override
    {
        SLANG_RETURN_ON_FAIL(CUDAShaderObject::collectSpecializationArgs(args));
        for (auto& entryPoint : entryPointObjects)
        {
            SLANG_RETURN_ON_FAIL(entryPoint->collectSpecializationArgs(args));
        }
        return SLANG_OK;
    }
};

class CUDAShaderProgram : public ShaderProgramBase
{
public:
    CUmodule cudaModule = nullptr;
    CUfunction cudaKernel;
    String kernelName;
    RefPtr<CUDAProgramLayout> layout;
    RefPtr<CUDAContext> cudaContext;
    ~CUDAShaderProgram()
    {
        if (cudaModule)
            cuModuleUnload(cudaModule);
    }
};

class CUDAPipelineState : public PipelineStateBase
{
public:
    RefPtr<CUDAShaderProgram> shaderProgram;
    void init(const ComputePipelineStateDesc& inDesc)
    {
        PipelineStateDesc pipelineDesc;
        pipelineDesc.type = PipelineType::Compute;
        pipelineDesc.compute = inDesc;
        initializeBase(pipelineDesc);
    }
};

class CUDAQueryPool : public QueryPoolBase
{
public:
    // The event object for each query. Owned by the pool.
    List<CUevent> m_events;

    // The event that marks the starting point.
    CUevent m_startEvent;

    Result init(const IQueryPool::Desc& desc)
    {
        cuEventCreate(&m_startEvent, 0);
        cuEventRecord(m_startEvent, 0);
        m_events.setCount(desc.count);
        for (SlangInt i = 0; i < m_events.getCount(); i++)
        {
            cuEventCreate(&m_events[i], 0);
        }
        return SLANG_OK;
    }

    ~CUDAQueryPool()
    {
        for (auto& e : m_events)
        {
            cuEventDestroy(e);
        }
        cuEventDestroy(m_startEvent);
    }

    virtual SLANG_NO_THROW Result SLANG_MCALL getResult(
        SlangInt queryIndex, SlangInt count, uint64_t* data) override
    {
        for (SlangInt i = 0; i < count; i++)
        {
            float time = 0.0f;
            cuEventSynchronize(m_events[i + queryIndex]);
            cuEventElapsedTime(&time, m_startEvent, m_events[i + queryIndex]);
            data[i] = (uint64_t)((double)time * 1000.0f);
        }
        return SLANG_OK;
    }
};

class CUDADevice : public RendererBase
{
private:
    static const CUDAReportStyle reportType = CUDAReportStyle::Normal;
    static int _calcSMCountPerMultiProcessor(int major, int minor)
    {
        // Defines for GPU Architecture types (using the SM version to determine
        // the # of cores per SM
        struct SMInfo
        {
            int sm; // 0xMm (hexadecimal notation), M = SM Major version, and m = SM minor version
            int coreCount;
        };

        static const SMInfo infos[] = {
            {0x30, 192},
            {0x32, 192},
            {0x35, 192},
            {0x37, 192},
            {0x50, 128},
            {0x52, 128},
            {0x53, 128},
            {0x60, 64},
            {0x61, 128},
            {0x62, 128},
            {0x70, 64},
            {0x72, 64},
            {0x75, 64}};

        const int sm = ((major << 4) + minor);
        for (Index i = 0; i < SLANG_COUNT_OF(infos); ++i)
        {
            if (infos[i].sm == sm)
            {
                return infos[i].coreCount;
            }
        }

        const auto& last = infos[SLANG_COUNT_OF(infos) - 1];

        // It must be newer presumably
        SLANG_ASSERT(sm > last.sm);

        // Default to the last entry
        return last.coreCount;
    }

    static SlangResult _findMaxFlopsDeviceIndex(int* outDeviceIndex)
    {
        int smPerMultiproc = 0;
        int maxPerfDevice = -1;
        int deviceCount = 0;
        int devicesProhibited = 0;

        uint64_t maxComputePerf = 0;
        SLANG_CUDA_RETURN_ON_FAIL(cudaGetDeviceCount(&deviceCount));

        // Find the best CUDA capable GPU device
        for (int currentDevice = 0; currentDevice < deviceCount; ++currentDevice)
        {
            int computeMode = -1, major = 0, minor = 0;
            SLANG_CUDA_RETURN_ON_FAIL(
                cudaDeviceGetAttribute(&computeMode, cudaDevAttrComputeMode, currentDevice));
            SLANG_CUDA_RETURN_ON_FAIL(
                cudaDeviceGetAttribute(&major, cudaDevAttrComputeCapabilityMajor, currentDevice));
            SLANG_CUDA_RETURN_ON_FAIL(
                cudaDeviceGetAttribute(&minor, cudaDevAttrComputeCapabilityMinor, currentDevice));

            // If this GPU is not running on Compute Mode prohibited,
            // then we can add it to the list
            if (computeMode != cudaComputeModeProhibited)
            {
                if (major == 9999 && minor == 9999)
                {
                    smPerMultiproc = 1;
                }
                else
                {
                    smPerMultiproc = _calcSMCountPerMultiProcessor(major, minor);
                }

                int multiProcessorCount = 0, clockRate = 0;
                SLANG_CUDA_RETURN_ON_FAIL(cudaDeviceGetAttribute(
                    &multiProcessorCount, cudaDevAttrMultiProcessorCount, currentDevice));
                SLANG_CUDA_RETURN_ON_FAIL(
                    cudaDeviceGetAttribute(&clockRate, cudaDevAttrClockRate, currentDevice));
                uint64_t compute_perf = uint64_t(multiProcessorCount) * smPerMultiproc * clockRate;

                if (compute_perf > maxComputePerf)
                {
                    maxComputePerf = compute_perf;
                    maxPerfDevice = currentDevice;
                }
            }
            else
            {
                devicesProhibited++;
            }
        }

        if (maxPerfDevice < 0)
        {
            return SLANG_FAIL;
        }

        *outDeviceIndex = maxPerfDevice;
        return SLANG_OK;
    }

    static SlangResult _initCuda(CUDAReportStyle reportType = CUDAReportStyle::Normal)
    {
        static CUresult res = cuInit(0);
        SLANG_CUDA_RETURN_WITH_REPORT_ON_FAIL(res, reportType);
        return SLANG_OK;
    }

private:
    int m_deviceIndex = -1;
    CUdevice m_device = 0;
    RefPtr<CUDAContext> m_context;
    DeviceInfo m_info;
    String m_adapterName;

public:
    virtual SLANG_NO_THROW Result SLANG_MCALL getNativeDeviceHandles(InteropHandles* outHandles) override
    {
        outHandles->handles[0].handleValue = (uint64_t)m_device;
        outHandles->handles[0].api = InteropHandleAPI::CUDA;
        return SLANG_OK;
    }

    class CommandQueueImpl;

    class CommandBufferImpl
        : public ICommandBuffer
        , public CommandWriter
        , public ComObject
    {
    public:
        SLANG_COM_OBJECT_IUNKNOWN_ALL
        ICommandBuffer* getInterface(const Guid& guid)
        {
            if (guid == GfxGUID::IID_ISlangUnknown || guid == GfxGUID::IID_ICommandBuffer)
                return static_cast<ICommandBuffer*>(this);
            return nullptr;
        }
    public:
        CUDADevice* m_device;

        void init(CUDADevice* device) { m_device = device; }
        virtual SLANG_NO_THROW void SLANG_MCALL encodeRenderCommands(
            IRenderPassLayout* renderPass,
            IFramebuffer* framebuffer,
            IRenderCommandEncoder** outEncoder) override
        {
            SLANG_UNUSED(renderPass);
            SLANG_UNUSED(framebuffer);
            *outEncoder = nullptr;
        }

        class ComputeCommandEncoderImpl
            : public IComputeCommandEncoder
        {
        public:
            CommandWriter* m_writer;
            CommandBufferImpl* m_commandBuffer;
            RefPtr<ShaderObjectBase> m_rootObject;
            virtual SLANG_NO_THROW void SLANG_MCALL endEncoding() override {}
            void init(CommandBufferImpl* cmdBuffer)
            {
                m_writer = cmdBuffer;
                m_commandBuffer = cmdBuffer;
            }

            virtual SLANG_NO_THROW Result SLANG_MCALL
                bindPipeline(IPipelineState* state, IShaderObject** outRootObject) override
            {
                m_writer->setPipelineState(state);
                PipelineStateBase* pipelineImpl = static_cast<PipelineStateBase*>(state);
                SLANG_RETURN_ON_FAIL(m_commandBuffer->m_device->createRootShaderObject(
                    pipelineImpl->m_program, m_rootObject.writeRef()));
                returnComPtr(outRootObject, m_rootObject);
                return SLANG_OK;
            }

            virtual SLANG_NO_THROW void SLANG_MCALL dispatchCompute(int x, int y, int z) override
            {
                m_writer->bindRootShaderObject(m_rootObject);
                m_writer->dispatchCompute(x, y, z);
            }

            virtual SLANG_NO_THROW void SLANG_MCALL writeTimestamp(IQueryPool* pool, SlangInt index) override
            {
                m_writer->writeTimestamp(pool, index);
            }

            virtual SLANG_NO_THROW void SLANG_MCALL
                dispatchComputeIndirect(IBufferResource* argBuffer, uint64_t offset) override
            {
                SLANG_UNIMPLEMENTED_X("dispatchComputeIndirect");
            }
        };

        ComputeCommandEncoderImpl m_computeCommandEncoder;
        virtual SLANG_NO_THROW void SLANG_MCALL
            encodeComputeCommands(IComputeCommandEncoder** outEncoder) override
        {
            m_computeCommandEncoder.init(this);
            *outEncoder = &m_computeCommandEncoder;
        }

        class ResourceCommandEncoderImpl
            : public IResourceCommandEncoder
        {
        public:
            CommandWriter* m_writer;

            void init(CommandBufferImpl* cmdBuffer)
            {
                m_writer = cmdBuffer;
            }

            virtual SLANG_NO_THROW void SLANG_MCALL endEncoding() override {}
            virtual SLANG_NO_THROW void SLANG_MCALL copyBuffer(
                IBufferResource* dst,
                size_t dstOffset,
                IBufferResource* src,
                size_t srcOffset,
                size_t size) override
            {
                m_writer->copyBuffer(dst, dstOffset, src, srcOffset, size);
            }

            virtual SLANG_NO_THROW void SLANG_MCALL textureBarrier(
                size_t count,
                ITextureResource* const* textures,
                ResourceState src,
                ResourceState dst) override
            {
            }

            virtual SLANG_NO_THROW void SLANG_MCALL bufferBarrier(
                size_t count,
                IBufferResource* const* buffers,
                ResourceState src,
                ResourceState dst) override
            {
            }

            virtual SLANG_NO_THROW void SLANG_MCALL
                uploadBufferData(IBufferResource* dst, size_t offset, size_t size, void* data) override
            {
                m_writer->uploadBufferData(dst, offset, size, data);
            }

            virtual SLANG_NO_THROW void SLANG_MCALL writeTimestamp(IQueryPool* pool, SlangInt index) override
            {
                m_writer->writeTimestamp(pool, index);
            }

            virtual SLANG_NO_THROW void SLANG_MCALL copyTexture(
                ITextureResource* dst,
                SubresourceRange dstSubresource,
                ITextureResource::Offset3D dstOffset,
                ITextureResource* src,
                SubresourceRange srcSubresource,
                ITextureResource::Offset3D srcOffset,
                ITextureResource::Size extent) override
            {
                SLANG_UNUSED(dst);
                SLANG_UNUSED(dstSubresource);
                SLANG_UNUSED(dstOffset);
                SLANG_UNUSED(src);
                SLANG_UNUSED(srcSubresource);
                SLANG_UNUSED(srcOffset);
                SLANG_UNUSED(extent);
                SLANG_UNIMPLEMENTED_X("copyTexture");
            }

            virtual SLANG_NO_THROW void SLANG_MCALL uploadTextureData(
                ITextureResource* dst,
                SubresourceRange subResourceRange,
                ITextureResource::Offset3D offset,
                ITextureResource::Size extent,
                ITextureResource::SubresourceData* subResourceData,
                size_t subResourceDataCount) override
            {
                SLANG_UNUSED(dst);
                SLANG_UNUSED(subResourceRange);
                SLANG_UNUSED(offset);
                SLANG_UNUSED(extent);
                SLANG_UNUSED(subResourceData);
                SLANG_UNUSED(subResourceDataCount);
                SLANG_UNIMPLEMENTED_X("uploadTextureData");
            }

            virtual SLANG_NO_THROW void SLANG_MCALL clearResourceView(
                IResourceView* view,
                ClearValue* clearValue,
                ClearResourceViewFlags::Enum flags) override
            {
                SLANG_UNUSED(view);
                SLANG_UNUSED(clearValue);
                SLANG_UNUSED(flags);
                SLANG_UNIMPLEMENTED_X("clearResourceView");
            }

            virtual SLANG_NO_THROW void SLANG_MCALL resolveResource(
                ITextureResource* source,
                SubresourceRange sourceRange,
                ITextureResource* dest,
                SubresourceRange destRange) override
            {
                SLANG_UNUSED(source);
                SLANG_UNUSED(sourceRange);
                SLANG_UNUSED(dest);
                SLANG_UNUSED(destRange);
                SLANG_UNIMPLEMENTED_X("resolveResource");
            }

            virtual SLANG_NO_THROW void SLANG_MCALL copyTextureToBuffer(
                IBufferResource* dst,
                size_t dstOffset,
                size_t dstSize,
                ITextureResource* src,
                SubresourceRange srcSubresource,
                ITextureResource::Offset3D srcOffset,
                ITextureResource::Size extent) override
            {
                SLANG_UNUSED(dst);
                SLANG_UNUSED(dstOffset);
                SLANG_UNUSED(dstSize);
                SLANG_UNUSED(src);
                SLANG_UNUSED(srcSubresource);
                SLANG_UNUSED(srcOffset);
                SLANG_UNUSED(extent);
                SLANG_UNIMPLEMENTED_X("copyTextureToBuffer");
            }

            virtual SLANG_NO_THROW void SLANG_MCALL textureSubresourceBarrier(
                ITextureResource* texture,
                SubresourceRange subresourceRange,
                ResourceState src,
                ResourceState dst) override
            {
                SLANG_UNUSED(texture);
                SLANG_UNUSED(subresourceRange);
                SLANG_UNUSED(src);
                SLANG_UNUSED(dst);
                SLANG_UNIMPLEMENTED_X("textureSubresourceBarrier");
            }
        };

        ResourceCommandEncoderImpl m_resourceCommandEncoder;

        virtual SLANG_NO_THROW void SLANG_MCALL
            encodeResourceCommands(IResourceCommandEncoder** outEncoder) override
        {
            m_resourceCommandEncoder.init(this);
            *outEncoder = &m_resourceCommandEncoder;
        }

        virtual SLANG_NO_THROW void SLANG_MCALL
            encodeRayTracingCommands(IRayTracingCommandEncoder** outEncoder) override
        {
            *outEncoder = nullptr;
        }

        virtual SLANG_NO_THROW void SLANG_MCALL close() override {}

        virtual SLANG_NO_THROW Result SLANG_MCALL
            getNativeHandle(NativeHandle* outHandle) override
        {
            *outHandle = 0;
            return SLANG_OK;
        }
    };

    class CommandQueueImpl
        : public ICommandQueue
        , public ComObject
    {
    public:
        SLANG_COM_OBJECT_IUNKNOWN_ALL
        ICommandQueue* getInterface(const Guid& guid)
        {
            if (guid == GfxGUID::IID_ISlangUnknown || guid == GfxGUID::IID_ICommandQueue)
                return static_cast<ICommandQueue*>(this);
            return nullptr;
        }

    public:
        RefPtr<CUDAPipelineState> currentPipeline;
        RefPtr<CUDARootShaderObject> currentRootObject;
        RefPtr<CUDADevice> renderer;
        CUstream stream;
        Desc m_desc;
    public:
        void init(CUDADevice* inRenderer)
        {
            renderer = inRenderer;
            m_desc.type = ICommandQueue::QueueType::Graphics;
            cuStreamCreate(&stream, 0);
        }
        ~CommandQueueImpl()
        {
            cuStreamSynchronize(stream);
            cuStreamDestroy(stream);
            currentPipeline = nullptr;
            currentRootObject = nullptr;
        }

    public:
        virtual SLANG_NO_THROW const Desc& SLANG_MCALL getDesc() override
        {
            return m_desc;
        }

        virtual SLANG_NO_THROW void SLANG_MCALL executeCommandBuffers(
            uint32_t count, ICommandBuffer* const* commandBuffers, IFence* fence, uint64_t valueToSignal) override
        {
            SLANG_UNUSED(valueToSignal);
            // TODO: implement fence.
            assert(fence == nullptr);
            for (uint32_t i = 0; i < count; i++)
            {
                execute(static_cast<CommandBufferImpl*>(commandBuffers[i]));
            }
        }

        virtual SLANG_NO_THROW void SLANG_MCALL waitOnHost() override
        {
            auto resultCode = cuStreamSynchronize(stream);
            if (resultCode != cudaSuccess)
                SLANG_CUDA_HANDLE_ERROR(resultCode);
        }

        virtual SLANG_NO_THROW Result SLANG_MCALL waitForFenceValuesOnDevice(
            uint32_t fenceCount, IFence** fences, uint64_t* waitValues) override
        {
            return SLANG_FAIL;
        }

        virtual SLANG_NO_THROW Result SLANG_MCALL
            getNativeHandle(NativeHandle* outHandle) override
        {
            *outHandle = (uint64_t)stream;
            return SLANG_OK;
        }

    public:
        void setPipelineState(IPipelineState* state)
        {
            currentPipeline = dynamic_cast<CUDAPipelineState*>(state);
        }

        Result bindRootShaderObject(IShaderObject* object)
        {
            currentRootObject = dynamic_cast<CUDARootShaderObject*>(object);
            if (currentRootObject)
                return SLANG_OK;
            return SLANG_E_INVALID_ARG;
        }

        void dispatchCompute(int x, int y, int z)
        {
            // Specialize the compute kernel based on the shader object bindings.
            RefPtr<PipelineStateBase> newPipeline;
            renderer->maybeSpecializePipeline(currentPipeline, currentRootObject, newPipeline);
            currentPipeline = static_cast<CUDAPipelineState*>(newPipeline.Ptr());

            // Find out thread group size from program reflection.
            auto& kernelName = currentPipeline->shaderProgram->kernelName;
            auto programLayout = static_cast<CUDAProgramLayout*>(currentRootObject->getLayout());
            int kernelId = programLayout->getKernelIndex(kernelName.getUnownedSlice());
            SLANG_ASSERT(kernelId != -1);
            UInt threadGroupSize[3];
            programLayout->getKernelThreadGroupSize(kernelId, threadGroupSize);

            int sharedSizeInBytes;
            cuFuncGetAttribute(
                &sharedSizeInBytes,
                CU_FUNC_ATTRIBUTE_SHARED_SIZE_BYTES,
                currentPipeline->shaderProgram->cudaKernel);

            // Copy global parameter data to the `SLANG_globalParams` symbol.
            {
                CUdeviceptr globalParamsSymbol = 0;
                size_t globalParamsSymbolSize = 0;
                cuModuleGetGlobal(
                    &globalParamsSymbol,
                    &globalParamsSymbolSize,
                    currentPipeline->shaderProgram->cudaModule,
                    "SLANG_globalParams");

                CUdeviceptr globalParamsCUDAData = (CUdeviceptr)currentRootObject->getBuffer();
                cudaMemcpyAsync(
                    (void*)globalParamsSymbol,
                    (void*)globalParamsCUDAData,
                    globalParamsSymbolSize,
                    cudaMemcpyDefault,
                    0);
            }
            //
            // The argument data for the entry-point parameters are already
            // stored in host memory in a CUDAEntryPointShaderObject, as expected by cuLaunchKernel.
            //
            auto entryPointBuffer = currentRootObject->entryPointObjects[kernelId]->getBuffer();
            auto entryPointDataSize =
                currentRootObject->entryPointObjects[kernelId]->getBufferSize();

            void* extraOptions[] = {
                CU_LAUNCH_PARAM_BUFFER_POINTER,
                entryPointBuffer,
                CU_LAUNCH_PARAM_BUFFER_SIZE,
                &entryPointDataSize,
                CU_LAUNCH_PARAM_END,
            };

            // Once we have all the decessary data extracted and/or
            // set up, we can launch the kernel and see what happens.
            //
            auto cudaLaunchResult = cuLaunchKernel(
                currentPipeline->shaderProgram->cudaKernel,
                x,
                y,
                z,
                int(threadGroupSize[0]),
                int(threadGroupSize[1]),
                int(threadGroupSize[2]),
                sharedSizeInBytes,
                stream,
                nullptr,
                extraOptions);

            SLANG_ASSERT(cudaLaunchResult == CUDA_SUCCESS);
        }

        void copyBuffer(
            IBufferResource* dst,
            size_t dstOffset,
            IBufferResource* src,
            size_t srcOffset,
            size_t size)
        {
            auto dstImpl = static_cast<MemoryCUDAResource*>(dst);
            auto srcImpl = static_cast<MemoryCUDAResource*>(src);
            cudaMemcpy(
                (uint8_t*)dstImpl->m_cudaMemory + dstOffset,
                (uint8_t*)srcImpl->m_cudaMemory + srcOffset,
                size,
                cudaMemcpyDefault);
        }

        void uploadBufferData(IBufferResource* dst, size_t offset, size_t size, void* data)
        {
            auto dstImpl = static_cast<MemoryCUDAResource*>(dst);
            cudaMemcpy((uint8_t*)dstImpl->m_cudaMemory + offset, data, size, cudaMemcpyDefault);
        }

        void writeTimestamp(IQueryPool* pool, SlangInt index)
        {
            auto poolImpl = static_cast<CUDAQueryPool*>(pool);
            cuEventRecord(poolImpl->m_events[index], stream);
        }

        void execute(CommandBufferImpl* commandBuffer)
        {
            for (auto& cmd : commandBuffer->m_commands)
            {
                switch (cmd.name)
                {
                case CommandName::SetPipelineState:
                    setPipelineState(commandBuffer->getObject<PipelineStateBase>(cmd.operands[0]));
                    break;
                case CommandName::BindRootShaderObject:
                    bindRootShaderObject(
                        commandBuffer->getObject<ShaderObjectBase>(cmd.operands[0]));
                    break;
                case CommandName::DispatchCompute:
                    dispatchCompute(
                        int(cmd.operands[0]), int(cmd.operands[1]), int(cmd.operands[2]));
                    break;
                case CommandName::CopyBuffer:
                    copyBuffer(
                        commandBuffer->getObject<BufferResource>(cmd.operands[0]),
                        cmd.operands[1],
                        commandBuffer->getObject<BufferResource>(cmd.operands[2]),
                        cmd.operands[3],
                        cmd.operands[4]);
                    break;
                case CommandName::UploadBufferData:
                    uploadBufferData(
                        commandBuffer->getObject<BufferResource>(cmd.operands[0]),
                        cmd.operands[1],
                        cmd.operands[2],
                        commandBuffer->getData<uint8_t>(cmd.operands[3]));
                    break;
                case CommandName::WriteTimestamp:
                    writeTimestamp(
                        commandBuffer->getObject<QueryPoolBase>(cmd.operands[0]),
                        (SlangInt)cmd.operands[1]);
                }
            }
        }
    };

    using TransientResourceHeapImpl = SimpleTransientResourceHeap<CUDADevice, CommandBufferImpl>;

public:
    virtual SLANG_NO_THROW SlangResult SLANG_MCALL initialize(const Desc& desc) override
    {
        SLANG_RETURN_ON_FAIL(slangContext.initialize(
            desc.slang,
            SLANG_PTX,
            "sm_5_1",
            makeArray(slang::PreprocessorMacroDesc{ "__CUDA_COMPUTE__", "1" }).getView()));

        SLANG_RETURN_ON_FAIL(RendererBase::initialize(desc));

        SLANG_RETURN_ON_FAIL(_initCuda(reportType));

        SLANG_RETURN_ON_FAIL(_findMaxFlopsDeviceIndex(&m_deviceIndex));
        SLANG_CUDA_RETURN_WITH_REPORT_ON_FAIL(cudaSetDevice(m_deviceIndex), reportType);

        m_context = new CUDAContext();

        int count = -1;
        cuDeviceGetCount(&count);
        SLANG_CUDA_RETURN_ON_FAIL(cuDeviceGet(&m_device, m_deviceIndex));

        SLANG_CUDA_RETURN_WITH_REPORT_ON_FAIL(
            cuCtxCreate(&m_context->m_context, 0, m_device), reportType);

        // Not clear how to detect half support on CUDA. For now we'll assume we have it
        {
            m_features.add("half");
        }

        // Initialize DeviceInfo
        {
            m_info.deviceType = DeviceType::CUDA;
            m_info.bindingStyle = BindingStyle::CUDA;
            m_info.projectionStyle = ProjectionStyle::DirectX;
            m_info.apiName = "CUDA";
            static const float kIdentity[] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
            ::memcpy(m_info.identityProjectionMatrix, kIdentity, sizeof(kIdentity));
            cudaDeviceProp deviceProperties;
            cudaGetDeviceProperties(&deviceProperties, m_deviceIndex);
            m_adapterName = deviceProperties.name;
            m_info.adapterName = m_adapterName.begin();
            m_info.timestampFrequency = 1000000;
        }

        return SLANG_OK;
    }

    Result getCUDAFormat(Format format, CUarray_format* outFormat)
    {
        // TODO: Expand to cover all available formats that can be supported in CUDA
        switch (format)
        {
        case Format::R32G32B32A32_FLOAT:
        case Format::R32G32B32_FLOAT:
        case Format::R32G32_FLOAT:
        case Format::R32_FLOAT:
        case Format::D32_FLOAT:
            *outFormat = CU_AD_FORMAT_FLOAT;
            return SLANG_OK;
        case Format::R16G16B16A16_FLOAT:
        case Format::R16G16_FLOAT:
        case Format::R16_FLOAT:
            *outFormat = CU_AD_FORMAT_HALF;
            return SLANG_OK;
        case Format::R32G32B32A32_UINT:
        case Format::R32G32B32_UINT:
        case Format::R32G32_UINT:
        case Format::R32_UINT:
            *outFormat = CU_AD_FORMAT_UNSIGNED_INT32;
            return SLANG_OK;
        case Format::R16G16B16A16_UINT:
        case Format::R16G16_UINT:
        case Format::R16_UINT:
            *outFormat = CU_AD_FORMAT_UNSIGNED_INT16;
            return SLANG_OK;
        case Format::R8G8B8A8_UINT:
        case Format::R8G8_UINT:
        case Format::R8_UINT:
        case Format::R8G8B8A8_UNORM:
            *outFormat = CU_AD_FORMAT_UNSIGNED_INT8;
            return SLANG_OK;
        case Format::R32G32B32A32_SINT:
        case Format::R32G32B32_SINT:
        case Format::R32G32_SINT:
        case Format::R32_SINT:
            *outFormat = CU_AD_FORMAT_SIGNED_INT32;
            return SLANG_OK;
        case Format::R16G16B16A16_SINT:
        case Format::R16G16_SINT:
        case Format::R16_SINT:
            *outFormat = CU_AD_FORMAT_SIGNED_INT16;
            return SLANG_OK;
        case Format::R8G8B8A8_SINT:
        case Format::R8G8_SINT:
        case Format::R8_SINT:
            *outFormat = CU_AD_FORMAT_SIGNED_INT8;
            return SLANG_OK;
        default:
            SLANG_ASSERT(!"Only support R32_FLOAT/R8G8B8A8_UNORM formats for now");
            return SLANG_FAIL;
        }
    }

    virtual SLANG_NO_THROW Result SLANG_MCALL createTextureResource(
        const ITextureResource::Desc& desc,
        const ITextureResource::SubresourceData* initData,
        ITextureResource** outResource) override
    {
        TextureResource::Desc srcDesc = fixupTextureDesc(desc);

        RefPtr<TextureCUDAResource> tex = new TextureCUDAResource(srcDesc);
        tex->m_cudaContext = m_context;

        CUresourcetype resourceType;

        // The size of the element/texel in bytes
        size_t elementSize = 0;

        // Our `ITextureResource::Desc` uses an enumeration to specify
        // the "shape"/rank of a texture (1D, 2D, 3D, Cube), but CUDA's
        // `cuMipmappedArrayCreate` seemingly relies on a policy where
        // the extents of the array in dimenions above the rank are
        // specified as zero (e.g., a 1D texture requires `height==0`).
        //
        // We will start by massaging the extents as specified by the
        // user into a form that CUDA wants/expects, based on the
        // texture shape as specified in the `desc`.
        //
        int width = desc.size.width;
        int height = desc.size.height;
        int depth = desc.size.depth;
        switch (desc.type)
        {
        case IResource::Type::Texture1D:
            height = 0;
            depth = 0;
            break;

        case IResource::Type::Texture2D:
            depth = 0;
            break;

        case IResource::Type::Texture3D:
            break;

        case IResource::Type::TextureCube:
            depth = 1;
            break;
        }

        {
            CUarray_format format = CU_AD_FORMAT_FLOAT;
            int numChannels = 0;

            SLANG_RETURN_ON_FAIL(getCUDAFormat(desc.format, &format));
            FormatInfo info;
            gfxGetFormatInfo(desc.format, &info);
            numChannels = info.channelCount;

            switch (format)
            {
            case CU_AD_FORMAT_FLOAT:
            {
                elementSize = sizeof(float) * numChannels;
                break;
            }
            case CU_AD_FORMAT_HALF:
            {
                elementSize = sizeof(uint16_t) * numChannels;
                break;
            }
            case CU_AD_FORMAT_UNSIGNED_INT8:
            {
                elementSize = sizeof(uint32_t) * numChannels;
                break;
            }
            default:
            {
                SLANG_ASSERT(!"Only support R32_FLOAT/R8G8B8A8_UNORM formats for now");
                return SLANG_FAIL;
            }
            }

            if (desc.numMipLevels > 1)
            {
                resourceType = CU_RESOURCE_TYPE_MIPMAPPED_ARRAY;

                CUDA_ARRAY3D_DESCRIPTOR arrayDesc;
                memset(&arrayDesc, 0, sizeof(arrayDesc));

                arrayDesc.Width = width;
                arrayDesc.Height = height;
                arrayDesc.Depth = depth;
                arrayDesc.Format = format;
                arrayDesc.NumChannels = numChannels;
                arrayDesc.Flags = 0;

                if (desc.arraySize > 1)
                {
                    if (desc.type == IResource::Type::Texture1D ||
                        desc.type == IResource::Type::Texture2D ||
                        desc.type == IResource::Type::TextureCube)
                    {
                        arrayDesc.Flags |= CUDA_ARRAY3D_LAYERED;
                        arrayDesc.Depth = desc.arraySize;
                    }
                    else
                    {
                        SLANG_ASSERT(!"Arrays only supported for 1D and 2D");
                        return SLANG_FAIL;
                    }
                }

                if (desc.type == IResource::Type::TextureCube)
                {
                    arrayDesc.Flags |= CUDA_ARRAY3D_CUBEMAP;
                    arrayDesc.Depth *= 6;
                }

                SLANG_CUDA_RETURN_ON_FAIL(
                    cuMipmappedArrayCreate(&tex->m_cudaMipMappedArray, &arrayDesc, desc.numMipLevels));
            }
            else
            {
                resourceType = CU_RESOURCE_TYPE_ARRAY;

                if (desc.arraySize > 1)
                {
                    if (desc.type == IResource::Type::Texture1D ||
                        desc.type == IResource::Type::Texture2D ||
                        desc.type == IResource::Type::TextureCube)
                    {
                        SLANG_ASSERT(!"Only 1D, 2D and Cube arrays supported");
                        return SLANG_FAIL;
                    }

                    CUDA_ARRAY3D_DESCRIPTOR arrayDesc;
                    memset(&arrayDesc, 0, sizeof(arrayDesc));

                    // Set the depth as the array length
                    arrayDesc.Depth = desc.arraySize;
                    if (desc.type == IResource::Type::TextureCube)
                    {
                        arrayDesc.Depth *= 6;
                    }

                    arrayDesc.Height = height;
                    arrayDesc.Width = width;
                    arrayDesc.Format = format;
                    arrayDesc.NumChannels = numChannels;

                    if (desc.type == IResource::Type::TextureCube)
                    {
                        arrayDesc.Flags |= CUDA_ARRAY3D_CUBEMAP;
                    }

                    SLANG_CUDA_RETURN_ON_FAIL(cuArray3DCreate(&tex->m_cudaArray, &arrayDesc));
                }
                else if (desc.type == IResource::Type::Texture3D ||
                    desc.type == IResource::Type::TextureCube)
                {
                    CUDA_ARRAY3D_DESCRIPTOR arrayDesc;
                    memset(&arrayDesc, 0, sizeof(arrayDesc));

                    arrayDesc.Depth = depth;
                    arrayDesc.Height = height;
                    arrayDesc.Width = width;
                    arrayDesc.Format = format;
                    arrayDesc.NumChannels = numChannels;

                    arrayDesc.Flags = 0;

                    // Handle cube texture
                    if (desc.type == IResource::Type::TextureCube)
                    {
                        arrayDesc.Depth = 6;
                        arrayDesc.Flags |= CUDA_ARRAY3D_CUBEMAP;
                    }

                    SLANG_CUDA_RETURN_ON_FAIL(cuArray3DCreate(&tex->m_cudaArray, &arrayDesc));
                }
                else
                {
                    CUDA_ARRAY_DESCRIPTOR arrayDesc;
                    memset(&arrayDesc, 0, sizeof(arrayDesc));

                    arrayDesc.Height = height;
                    arrayDesc.Width = width;
                    arrayDesc.Format = format;
                    arrayDesc.NumChannels = numChannels;

                    // Allocate the array, will work for 1D or 2D case
                    SLANG_CUDA_RETURN_ON_FAIL(cuArrayCreate(&tex->m_cudaArray, &arrayDesc));
                }
            }
        }

        // Work space for holding data for uploading if it needs to be rearranged
        if (initData)
        {
            List<uint8_t> workspace;
            for (int mipLevel = 0; mipLevel < desc.numMipLevels; ++mipLevel)
            {
                int mipWidth = width >> mipLevel;
                int mipHeight = height >> mipLevel;
                int mipDepth = depth >> mipLevel;

                mipWidth = (mipWidth == 0) ? 1 : mipWidth;
                mipHeight = (mipHeight == 0) ? 1 : mipHeight;
                mipDepth = (mipDepth == 0) ? 1 : mipDepth;

                // If it's a cubemap then the depth is always 6
                if (desc.type == IResource::Type::TextureCube)
                {
                    mipDepth = 6;
                }

                auto dstArray = tex->m_cudaArray;
                if (tex->m_cudaMipMappedArray)
                {
                    // Get the array for the mip level
                    SLANG_CUDA_RETURN_ON_FAIL(
                        cuMipmappedArrayGetLevel(&dstArray, tex->m_cudaMipMappedArray, mipLevel));
                }
                SLANG_ASSERT(dstArray);

                // Check using the desc to see if it's plausible
                {
                    CUDA_ARRAY_DESCRIPTOR arrayDesc;
                    SLANG_CUDA_RETURN_ON_FAIL(cuArrayGetDescriptor(&arrayDesc, dstArray));

                    SLANG_ASSERT(mipWidth == arrayDesc.Width);
                    SLANG_ASSERT(
                        mipHeight == arrayDesc.Height || (mipHeight == 1 && arrayDesc.Height == 0));
                }

                const void* srcDataPtr = nullptr;

                if (desc.arraySize > 1)
                {
                    SLANG_ASSERT(
                        desc.type == IResource::Type::Texture1D ||
                        desc.type == IResource::Type::Texture2D ||
                        desc.type == IResource::Type::TextureCube);

                    // TODO(JS): Here I assume that arrays are just held contiguously within a
                    // 'face' This seems reasonable and works with the Copy3D.
                    const size_t faceSizeInBytes = elementSize * mipWidth * mipHeight;

                    Index faceCount = desc.arraySize;
                    if (desc.type == IResource::Type::TextureCube)
                    {
                        faceCount *= 6;
                    }

                    const size_t mipSizeInBytes = faceSizeInBytes * faceCount;
                    workspace.setCount(mipSizeInBytes);

                    // We need to add the face data from each mip
                    // We iterate over face count so we copy all of the cubemap faces
                    for (Index j = 0; j < faceCount; j++)
                    {
                        const auto srcData = initData[mipLevel + j * desc.numMipLevels].data;
                        // Copy over to the workspace to make contiguous
                        ::memcpy(
                            workspace.begin() + faceSizeInBytes * j, srcData, faceSizeInBytes);
                    }

                    srcDataPtr = workspace.getBuffer();
                }
                else
                {
                    if (desc.type == IResource::Type::TextureCube)
                    {
                        size_t faceSizeInBytes = elementSize * mipWidth * mipHeight;

                        workspace.setCount(faceSizeInBytes * 6);
                        // Copy the data over to make contiguous
                        for (Index j = 0; j < 6; j++)
                        {
                            const auto srcData =
                                initData[mipLevel + j * desc.numMipLevels].data;
                            ::memcpy(
                                workspace.getBuffer() + faceSizeInBytes * j,
                                srcData,
                                faceSizeInBytes);
                        }
                        srcDataPtr = workspace.getBuffer();
                    }
                    else
                    {
                        const auto srcData = initData[mipLevel].data;
                        srcDataPtr = srcData;
                    }
                }

                if (desc.arraySize > 1)
                {
                    SLANG_ASSERT(
                        desc.type == IResource::Type::Texture1D ||
                        desc.type == IResource::Type::Texture2D ||
                        desc.type == IResource::Type::TextureCube);

                    CUDA_MEMCPY3D copyParam;
                    memset(&copyParam, 0, sizeof(copyParam));

                    copyParam.dstMemoryType = CU_MEMORYTYPE_ARRAY;
                    copyParam.dstArray = dstArray;

                    copyParam.srcMemoryType = CU_MEMORYTYPE_HOST;
                    copyParam.srcHost = srcDataPtr;
                    copyParam.srcPitch = mipWidth * elementSize;
                    copyParam.WidthInBytes = copyParam.srcPitch;
                    copyParam.Height = mipHeight;
                    // Set the depth to the array length
                    copyParam.Depth = desc.arraySize;

                    if (desc.type == IResource::Type::TextureCube)
                    {
                        copyParam.Depth *= 6;
                    }

                    SLANG_CUDA_RETURN_ON_FAIL(cuMemcpy3D(&copyParam));
                }
                else
                {
                    switch (desc.type)
                    {
                    case IResource::Type::Texture1D:
                    case IResource::Type::Texture2D:
                        {
                            CUDA_MEMCPY2D copyParam;
                            memset(&copyParam, 0, sizeof(copyParam));
                            copyParam.dstMemoryType = CU_MEMORYTYPE_ARRAY;
                            copyParam.dstArray = dstArray;
                            copyParam.srcMemoryType = CU_MEMORYTYPE_HOST;
                            copyParam.srcHost = srcDataPtr;
                            copyParam.srcPitch = mipWidth * elementSize;
                            copyParam.WidthInBytes = copyParam.srcPitch;
                            copyParam.Height = mipHeight;
                            SLANG_CUDA_RETURN_ON_FAIL(cuMemcpy2D(&copyParam));
                            break;
                        }
                    case IResource::Type::Texture3D:
                    case IResource::Type::TextureCube:
                        {
                            CUDA_MEMCPY3D copyParam;
                            memset(&copyParam, 0, sizeof(copyParam));

                            copyParam.dstMemoryType = CU_MEMORYTYPE_ARRAY;
                            copyParam.dstArray = dstArray;

                            copyParam.srcMemoryType = CU_MEMORYTYPE_HOST;
                            copyParam.srcHost = srcDataPtr;
                            copyParam.srcPitch = mipWidth * elementSize;
                            copyParam.WidthInBytes = copyParam.srcPitch;
                            copyParam.Height = mipHeight;
                            copyParam.Depth = mipDepth;

                            SLANG_CUDA_RETURN_ON_FAIL(cuMemcpy3D(&copyParam));
                            break;
                        }

                    default:
                        {
                            SLANG_ASSERT(!"Not implemented");
                            break;
                        }
                    }
                }
            }
        }
        // Set up texture sampling parameters, and create final texture obj

        {
            CUDA_RESOURCE_DESC resDesc;
            memset(&resDesc, 0, sizeof(CUDA_RESOURCE_DESC));
            resDesc.resType = resourceType;

            if (tex->m_cudaArray)
            {
                resDesc.res.array.hArray = tex->m_cudaArray;
            }
            if (tex->m_cudaMipMappedArray)
            {
                resDesc.res.mipmap.hMipmappedArray = tex->m_cudaMipMappedArray;
            }

            // If the texture might be used as a UAV, then we need to allocate
            // a CUDA "surface" for it.
            //
            // Note: We cannot do this unconditionally, because it will fail
            // on surfaces that are not usable as UAVs (e.g., those with
            // mipmaps).
            //
            // TODO: We should really only be allocating the array at the
            // time we create a resource, and then allocate the surface or
            // texture objects as part of view creation.
            //
            if (desc.allowedStates.contains(ResourceState::UnorderedAccess))
            {
                // On CUDA surfaces only support a single MIP map
                SLANG_ASSERT(desc.numMipLevels == 1);

                SLANG_CUDA_RETURN_ON_FAIL(cuSurfObjectCreate(&tex->m_cudaSurfObj, &resDesc));
            }

            
            // Create handle for sampling.
            CUDA_TEXTURE_DESC texDesc;
            memset(&texDesc, 0, sizeof(CUDA_TEXTURE_DESC));
            texDesc.addressMode[0] = CU_TR_ADDRESS_MODE_WRAP;
            texDesc.addressMode[1] = CU_TR_ADDRESS_MODE_WRAP;
            texDesc.addressMode[2] = CU_TR_ADDRESS_MODE_WRAP;
            texDesc.filterMode = CU_TR_FILTER_MODE_LINEAR;
            texDesc.flags = CU_TRSF_NORMALIZED_COORDINATES;

            SLANG_CUDA_RETURN_ON_FAIL(
                cuTexObjectCreate(&tex->m_cudaTexObj, &resDesc, &texDesc, nullptr));
        }

        returnComPtr(outResource, tex);
        return SLANG_OK;
    }

    virtual SLANG_NO_THROW Result SLANG_MCALL createBufferResource(
        const IBufferResource::Desc& descIn,
        const void* initData,
        IBufferResource** outResource) override
    {
        auto desc = fixupBufferDesc(descIn);
        RefPtr<MemoryCUDAResource> resource = new MemoryCUDAResource(desc);
        resource->m_cudaContext = m_context;
        SLANG_CUDA_RETURN_ON_FAIL(cudaMallocManaged(&resource->m_cudaMemory, desc.sizeInBytes));
        if (initData)
        {
            SLANG_CUDA_RETURN_ON_FAIL(cudaMemcpy(resource->m_cudaMemory, initData, desc.sizeInBytes, cudaMemcpyDefault));
        }
        returnComPtr(outResource, resource);
        return SLANG_OK;
    }

    virtual SLANG_NO_THROW Result SLANG_MCALL createBufferFromSharedHandle(
        InteropHandle handle,
        const IBufferResource::Desc& desc,
        IBufferResource** outResource) override
    {
        if (handle.handleValue == 0)
        {
            *outResource = nullptr;
            return SLANG_OK;
        }

        RefPtr<MemoryCUDAResource> resource = new MemoryCUDAResource(desc);
        resource->m_cudaContext = m_context;

        // CUDA manages sharing of buffers through the idea of an
        // "external memory" object, which represents the relationship
        // with another API's objects. In order to create this external
        // memory association, we first need to fill in a descriptor struct.
        cudaExternalMemoryHandleDesc externalMemoryHandleDesc;
        memset(&externalMemoryHandleDesc, 0, sizeof(externalMemoryHandleDesc));
        switch (handle.api)
        {
        case InteropHandleAPI::D3D12:
            externalMemoryHandleDesc.type = cudaExternalMemoryHandleTypeD3D12Resource;
            break;
        case InteropHandleAPI::Vulkan:
            externalMemoryHandleDesc.type = cudaExternalMemoryHandleTypeOpaqueWin32;
            break;
        default:
            return SLANG_FAIL;
        }
        externalMemoryHandleDesc.handle.win32.handle = (void*)handle.handleValue;
        externalMemoryHandleDesc.size = desc.sizeInBytes;
        externalMemoryHandleDesc.flags = cudaExternalMemoryDedicated;

        // Once we have filled in the descriptor, we can request
        // that CUDA create the required association between the
        // external buffer and its own memory.
        cudaExternalMemory_t externalMemory;
        SLANG_CUDA_RETURN_ON_FAIL(cudaImportExternalMemory(&externalMemory, &externalMemoryHandleDesc));
        resource->m_cudaExternalMemory = externalMemory;

        // The CUDA "external memory" handle is not itself a device
        // pointer, so we need to query for a suitable device address
        // for the buffer with another call.
        //
        // Just as for the external memory, we fill in a descriptor
        // structure (although in this case we only need to specify
        // the size).
        cudaExternalMemoryBufferDesc bufferDesc;
        memset(&bufferDesc, 0, sizeof(bufferDesc));
        bufferDesc.size = desc.sizeInBytes;

        // Finally, we can "map" the buffer to get a device address.
        void* deviceAddress;
        SLANG_CUDA_RETURN_ON_FAIL(cudaExternalMemoryGetMappedBuffer(&deviceAddress, externalMemory, &bufferDesc));
        resource->m_cudaMemory = deviceAddress;

        returnComPtr(outResource, resource);
        return SLANG_OK;
    }

    virtual SLANG_NO_THROW Result SLANG_MCALL createTextureFromSharedHandle(
        InteropHandle handle,
        const ITextureResource::Desc& desc,
        const size_t size,
        ITextureResource** outResource) override
    {
        if (handle.handleValue == 0)
        {
            *outResource = nullptr;
            return SLANG_OK;
        }

        RefPtr<TextureCUDAResource> resource = new TextureCUDAResource(desc);
        resource->m_cudaContext = m_context;

        // CUDA manages sharing of buffers through the idea of an
        // "external memory" object, which represents the relationship
        // with another API's objects. In order to create this external
        // memory association, we first need to fill in a descriptor struct.
        CUDA_EXTERNAL_MEMORY_HANDLE_DESC externalMemoryHandleDesc;
        memset(&externalMemoryHandleDesc, 0, sizeof(externalMemoryHandleDesc));
        switch (handle.api)
        {
        case InteropHandleAPI::D3D12:
            externalMemoryHandleDesc.type = CU_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE;
            break;
        case InteropHandleAPI::Vulkan:
            externalMemoryHandleDesc.type = CU_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32;
            break;
        default:
            return SLANG_FAIL;
        }
        externalMemoryHandleDesc.handle.win32.handle = (void*)handle.handleValue;
        externalMemoryHandleDesc.size = size;
        externalMemoryHandleDesc.flags = cudaExternalMemoryDedicated;

        CUexternalMemory externalMemory;
        SLANG_CUDA_RETURN_ON_FAIL(cuImportExternalMemory(&externalMemory, &externalMemoryHandleDesc));
        resource->m_cudaExternalMemory = externalMemory;

        FormatInfo formatInfo;
        SLANG_RETURN_ON_FAIL(gfxGetFormatInfo(desc.format, &formatInfo));
        CUDA_ARRAY3D_DESCRIPTOR arrayDesc;
        arrayDesc.Depth = desc.size.depth;
        arrayDesc.Height = desc.size.height;
        arrayDesc.Width = desc.size.width;
        arrayDesc.NumChannels = formatInfo.channelCount;
        getCUDAFormat(desc.format, &arrayDesc.Format);
        arrayDesc.Flags = 0; // TODO: Flags? CUDA_ARRAY_LAYERED/SURFACE_LDST/CUBEMAP/TEXTURE_GATHER

        CUDA_EXTERNAL_MEMORY_MIPMAPPED_ARRAY_DESC externalMemoryMipDesc;
        memset(&externalMemoryMipDesc, 0, sizeof(externalMemoryMipDesc));
        externalMemoryMipDesc.offset = 0;
        externalMemoryMipDesc.arrayDesc = arrayDesc;
        externalMemoryMipDesc.numLevels = desc.numMipLevels;

        CUmipmappedArray mipArray;
        SLANG_CUDA_RETURN_ON_FAIL(cuExternalMemoryGetMappedMipmappedArray(&mipArray, externalMemory, &externalMemoryMipDesc));
        resource->m_cudaMipMappedArray = mipArray;

        CUarray cuArray;
        SLANG_CUDA_RETURN_ON_FAIL(cuMipmappedArrayGetLevel(&cuArray, mipArray, 0));
        resource->m_cudaArray = cuArray;

        CUDA_RESOURCE_DESC surfDesc;
        memset(&surfDesc, 0, sizeof(surfDesc));
        surfDesc.resType = CU_RESOURCE_TYPE_ARRAY;
        surfDesc.res.array.hArray = cuArray;

        CUsurfObject surface;
        SLANG_CUDA_RETURN_ON_FAIL(cuSurfObjectCreate(&surface, &surfDesc));
        resource->m_cudaSurfObj = surface;

        returnComPtr(outResource, resource);
        return SLANG_OK;
    }

    virtual SLANG_NO_THROW Result SLANG_MCALL createTextureView(
        ITextureResource* texture, IResourceView::Desc const& desc, IResourceView** outView) override
    {
        RefPtr<CUDAResourceView> view = new CUDAResourceView();
        view->m_desc = desc;
        view->textureResource = dynamic_cast<TextureCUDAResource*>(texture);
        returnComPtr(outView, view);
        return SLANG_OK;
    }

    virtual SLANG_NO_THROW Result SLANG_MCALL createBufferView(
        IBufferResource* buffer, IResourceView::Desc const& desc, IResourceView** outView) override
    {
        RefPtr<CUDAResourceView> view = new CUDAResourceView();
        view->m_desc = desc;
        view->memoryResource = dynamic_cast<MemoryCUDAResource*>(buffer);
        returnComPtr(outView, view);
        return SLANG_OK;
    }

    virtual SLANG_NO_THROW Result SLANG_MCALL createQueryPool(
        const IQueryPool::Desc& desc,
        IQueryPool** outPool) override
    {
        RefPtr<CUDAQueryPool> pool = new CUDAQueryPool();
        SLANG_RETURN_ON_FAIL(pool->init(desc));
        returnComPtr(outPool, pool);
        return SLANG_OK;
    }

    virtual Result createShaderObjectLayout(
        slang::TypeLayoutReflection*    typeLayout,
        ShaderObjectLayoutBase**        outLayout) override
    {
        RefPtr<CUDAShaderObjectLayout> cudaLayout;
        cudaLayout = new CUDAShaderObjectLayout(this, typeLayout);
        returnRefPtrMove(outLayout, cudaLayout);
        return SLANG_OK;
    }

    virtual Result createShaderObject(
        ShaderObjectLayoutBase* layout,
        IShaderObject**         outObject) override
    {
        RefPtr<CUDAShaderObject> result = new CUDAShaderObject();
        SLANG_RETURN_ON_FAIL(result->init(this, dynamic_cast<CUDAShaderObjectLayout*>(layout)));
        returnComPtr(outObject, result);
        return SLANG_OK;
    }

    virtual Result createMutableShaderObject(
        ShaderObjectLayoutBase* layout,
        IShaderObject** outObject) override
    {
        RefPtr<CUDAMutableShaderObject> result = new CUDAMutableShaderObject();
        SLANG_RETURN_ON_FAIL(result->init(this, dynamic_cast<CUDAShaderObjectLayout*>(layout)));
        returnComPtr(outObject, result);
        return SLANG_OK;
    }

    Result createRootShaderObject(IShaderProgram* program, ShaderObjectBase** outObject)
    {
        auto cudaProgram = dynamic_cast<CUDAShaderProgram*>(program);
        auto cudaLayout = cudaProgram->layout;

        RefPtr<CUDARootShaderObject> result = new CUDARootShaderObject();
        SLANG_RETURN_ON_FAIL(result->init(this, cudaLayout));
        returnRefPtrMove(outObject, result);
        return SLANG_OK;
    }

    virtual SLANG_NO_THROW Result SLANG_MCALL
        createProgram(const IShaderProgram::Desc& desc, IShaderProgram** outProgram) override
    {
        // If this is a specializable program, we just keep a reference to the slang program and
        // don't actually create any kernels. This program will be specialized later when we know
        // the shader object bindings.
        RefPtr<CUDAShaderProgram> cudaProgram = new CUDAShaderProgram();
        cudaProgram->slangProgram = desc.slangProgram;
        cudaProgram->cudaContext = m_context;
        if (desc.slangProgram->getSpecializationParamCount() != 0)
        {
            cudaProgram->layout = new CUDAProgramLayout(this, desc.slangProgram->getLayout());
            returnComPtr(outProgram, cudaProgram);
            return SLANG_OK;
        }

        ComPtr<ISlangBlob> kernelCode;
        ComPtr<ISlangBlob> diagnostics;
        auto compileResult = desc.slangProgram->getEntryPointCode(
            (SlangInt)0, 0, kernelCode.writeRef(), diagnostics.writeRef());
        if (diagnostics)
        {
            getDebugCallback()->handleMessage(
                compileResult == SLANG_OK ? DebugMessageType::Warning : DebugMessageType::Error,
                DebugMessageSource::Slang,
                (char*)diagnostics->getBufferPointer());
        }
        SLANG_RETURN_ON_FAIL(compileResult);
        
        SLANG_CUDA_RETURN_ON_FAIL(cuModuleLoadData(&cudaProgram->cudaModule, kernelCode->getBufferPointer()));
        cudaProgram->kernelName = desc.slangProgram->getLayout()->getEntryPointByIndex(0)->getName();
        SLANG_CUDA_RETURN_ON_FAIL(cuModuleGetFunction(
            &cudaProgram->cudaKernel, cudaProgram->cudaModule, cudaProgram->kernelName.getBuffer()));

        auto slangProgram = desc.slangProgram;
        if( slangProgram )
        {
            cudaProgram->slangProgram = slangProgram;

            auto slangProgramLayout = slangProgram->getLayout();
            if(!slangProgramLayout)
                return SLANG_FAIL;

            RefPtr<CUDAProgramLayout> cudaLayout;
            cudaLayout = new CUDAProgramLayout(this, slangProgramLayout);
            cudaLayout->programLayout = slangProgramLayout;
            cudaProgram->layout = cudaLayout;
        }

        returnComPtr(outProgram, cudaProgram);
        return SLANG_OK;
    }

    virtual SLANG_NO_THROW Result SLANG_MCALL createComputePipelineState(
        const ComputePipelineStateDesc& desc, IPipelineState** outState) override
    {
        RefPtr<CUDAPipelineState> state = new CUDAPipelineState();
        state->shaderProgram = static_cast<CUDAShaderProgram*>(desc.program);
        state->init(desc);
        returnComPtr(outState, state);
        return Result();
    }

    void* map(IBufferResource* buffer)
    {
        return static_cast<MemoryCUDAResource*>(buffer)->m_cudaMemory;
    }

    void unmap(IBufferResource* buffer)
    {
        SLANG_UNUSED(buffer);
    }

    virtual SLANG_NO_THROW const DeviceInfo& SLANG_MCALL getDeviceInfo() const override
    {
        return m_info;
    }

public:
    virtual SLANG_NO_THROW Result SLANG_MCALL createTransientResourceHeap(
        const ITransientResourceHeap::Desc& desc,
        ITransientResourceHeap** outHeap) override
    {
        RefPtr<TransientResourceHeapImpl> result = new TransientResourceHeapImpl();
        SLANG_RETURN_ON_FAIL(result->init(this, desc));
        returnComPtr(outHeap, result);
        return SLANG_OK;
    }

    virtual SLANG_NO_THROW Result SLANG_MCALL
        createCommandQueue(const ICommandQueue::Desc& desc, ICommandQueue** outQueue) override
    {
        RefPtr<CommandQueueImpl> queue = new CommandQueueImpl();
        queue->init(this);
        returnComPtr(outQueue, queue);
        return SLANG_OK;
    }
    virtual SLANG_NO_THROW Result SLANG_MCALL createSwapchain(
        const ISwapchain::Desc& desc, WindowHandle window, ISwapchain** outSwapchain) override
    {
        SLANG_UNUSED(desc);
        SLANG_UNUSED(window);
        SLANG_UNUSED(outSwapchain);
        return SLANG_FAIL;
    }
    virtual SLANG_NO_THROW Result SLANG_MCALL createFramebufferLayout(
        const IFramebufferLayout::Desc& desc, IFramebufferLayout** outLayout) override
    {
        SLANG_UNUSED(desc);
        SLANG_UNUSED(outLayout);
        return SLANG_FAIL;
    }
    virtual SLANG_NO_THROW Result SLANG_MCALL
        createFramebuffer(const IFramebuffer::Desc& desc, IFramebuffer** outFramebuffer) override
    {
        SLANG_UNUSED(desc);
        SLANG_UNUSED(outFramebuffer);
        return SLANG_FAIL;
    }
    virtual SLANG_NO_THROW Result SLANG_MCALL createRenderPassLayout(
        const IRenderPassLayout::Desc& desc,
        IRenderPassLayout** outRenderPassLayout) override
    {
        SLANG_UNUSED(desc);
        SLANG_UNUSED(outRenderPassLayout);
        return SLANG_FAIL;
    }
    virtual SLANG_NO_THROW Result SLANG_MCALL
        createSamplerState(ISamplerState::Desc const& desc, ISamplerState** outSampler) override
    {
        SLANG_UNUSED(desc);
        *outSampler = nullptr;
        return SLANG_OK;
    }
    
    virtual SLANG_NO_THROW Result SLANG_MCALL createInputLayout(
        const InputElementDesc* inputElements,
        UInt inputElementCount,
        IInputLayout** outLayout) override
    {
        SLANG_UNUSED(inputElements);
        SLANG_UNUSED(inputElementCount);
        SLANG_UNUSED(outLayout);
        return SLANG_E_NOT_AVAILABLE;
    }

    virtual SLANG_NO_THROW Result SLANG_MCALL createGraphicsPipelineState(
        const GraphicsPipelineStateDesc& desc, IPipelineState** outState) override
    {
        SLANG_UNUSED(desc);
        SLANG_UNUSED(outState);
        return SLANG_E_NOT_AVAILABLE;
    }

    virtual SLANG_NO_THROW SlangResult SLANG_MCALL readTextureResource(
        ITextureResource* texture,
        ResourceState state,
        ISlangBlob** outBlob,
        size_t* outRowPitch,
        size_t* outPixelSize) override
    {
        auto textureImpl = static_cast<TextureCUDAResource*>(texture);
        RefPtr<ListBlob> blob = new ListBlob();

        auto desc = textureImpl->getDesc();
        auto width = desc->size.width;
        auto height = desc->size.height;
        FormatInfo sizeInfo;
        SLANG_RETURN_ON_FAIL(gfxGetFormatInfo(desc->format, &sizeInfo));
        size_t pixelSize = sizeInfo.blockSizeInBytes / sizeInfo.pixelsPerBlock;
        size_t rowPitch = width * pixelSize;
        size_t size = height * rowPitch;
        blob->m_data.setCount((Index)size);

        CUDA_MEMCPY2D copyParam;
        memset(&copyParam, 0, sizeof(copyParam));

        copyParam.srcMemoryType = CU_MEMORYTYPE_ARRAY;
        copyParam.srcArray = textureImpl->m_cudaArray;

        copyParam.dstMemoryType = CU_MEMORYTYPE_HOST;
        copyParam.dstHost = blob->m_data.getBuffer();
        copyParam.dstPitch = rowPitch;
        copyParam.WidthInBytes = copyParam.dstPitch;
        copyParam.Height = height;
        SLANG_CUDA_RETURN_ON_FAIL(cuMemcpy2D(&copyParam));

        *outRowPitch = rowPitch;
        *outPixelSize = pixelSize;
        returnComPtr(outBlob, blob);
        return SLANG_OK;
    }

    virtual SLANG_NO_THROW Result SLANG_MCALL readBufferResource(
        IBufferResource* buffer,
        size_t offset,
        size_t size,
        ISlangBlob** outBlob) override
    {
        auto bufferImpl = static_cast<MemoryCUDAResource*>(buffer);
        RefPtr<ListBlob> blob = new ListBlob();
        blob->m_data.setCount((Index)size);
        cudaMemcpy(
            blob->m_data.getBuffer(),
            (uint8_t*)bufferImpl->m_cudaMemory + offset,
            size,
            cudaMemcpyDefault);
        returnComPtr(outBlob, blob);
        return SLANG_OK;
    }
};

SlangResult CUDAShaderObject::init(IDevice* device, CUDAShaderObjectLayout* typeLayout)
{
    m_layout = typeLayout;

    // If the layout tells us that there is any uniform data,
    // then we need to allocate a constant buffer to hold that data.
    //
    // TODO: Do we need to allocate a shadow copy for use from
    // the CPU?
    //
    // TODO: When/where do we bind this constant buffer into
    // a descriptor set for later use?
    //
    auto slangLayout = getLayout()->getElementTypeLayout();
    size_t uniformSize = slangLayout->getSize();
    if (uniformSize)
    {
        m_data.setCount((Index)uniformSize);
    }

    // If the layout specifies that we have any resources or sub-objects,
    // then we need to size the appropriate arrays to account for them.
    //
    // Note: the counts here are the *total* number of resources/sub-objects
    // and not just the number of resource/sub-object ranges.
    //
    resources.setCount(typeLayout->getResourceCount());
    m_objects.setCount(typeLayout->getSubObjectCount());

    for (auto subObjectRange : getLayout()->subObjectRanges)
    {
        RefPtr<CUDAShaderObjectLayout> subObjectLayout = subObjectRange.layout;

        // In the case where the sub-object range represents an
        // existential-type leaf field (e.g., an `IBar`), we
        // cannot pre-allocate the object(s) to go into that
        // range, since we can't possibly know what to allocate
        // at this point.
        //
        if (!subObjectLayout)
            continue;
        //
        // Otherwise, we will allocate a sub-object to fill
        // in each entry in this range, based on the layout
        // information we already have.

        auto& bindingRangeInfo = getLayout()->m_bindingRanges[subObjectRange.bindingRangeIndex];
        for (Index i = 0; i < bindingRangeInfo.count; ++i)
        {
            RefPtr<CUDAShaderObject> subObject = new CUDAShaderObject();
            SLANG_RETURN_ON_FAIL(subObject->init(device, subObjectLayout));

            ShaderOffset offset;
            offset.uniformOffset = bindingRangeInfo.uniformOffset + sizeof(void*) * i;
            offset.bindingRangeIndex = subObjectRange.bindingRangeIndex;
            offset.bindingArrayIndex = i;

            SLANG_RETURN_ON_FAIL(setObject(offset, subObject));
        }
    }
    return SLANG_OK;
}

SlangResult CUDARootShaderObject::init(IDevice* device, CUDAShaderObjectLayout* typeLayout)
{
    SLANG_RETURN_ON_FAIL(CUDAShaderObject::init(device, typeLayout));
    auto programLayout = dynamic_cast<CUDAProgramLayout*>(typeLayout);
    for (auto& entryPoint : programLayout->entryPointLayouts)
    {
        RefPtr<CUDAEntryPointShaderObject> object = new CUDAEntryPointShaderObject();
        SLANG_RETURN_ON_FAIL(object->init(device, entryPoint));
        entryPointObjects.add(object);
    }
    return SLANG_OK;
}

SlangResult SLANG_MCALL createCUDADevice(const IDevice::Desc* desc, IDevice** outDevice)
{
    RefPtr<CUDADevice> result = new CUDADevice();
    SLANG_RETURN_ON_FAIL(result->initialize(*desc));
    returnComPtr(outDevice, result);
    return SLANG_OK;
}
#else
SlangResult SLANG_MCALL createCUDADevice(const IDevice::Desc* desc, IDevice** outDevice)
{
    SLANG_UNUSED(desc);
    *outDevice = nullptr;
    return SLANG_FAIL;
}
#endif

}
