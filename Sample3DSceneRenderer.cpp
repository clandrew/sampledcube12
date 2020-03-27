﻿#include "pch.h"
#include "Sample3DSceneRenderer.h"

#include "Common\DirectXHelper.h"

#include "SampleVertexShader.hlsl.h"
#include "SamplePixelShader.hlsl.h"

using namespace SpinningCube;

using namespace DirectX;
using namespace Microsoft::WRL;

// Loads vertex and pixel shaders from files and instantiates the cube geometry.
Sample3DSceneRenderer::Sample3DSceneRenderer(const std::shared_ptr<DX::DeviceResources>& deviceResources) :
	m_loadingComplete(false),
	m_radiansPerSecond(XM_PIDIV4),	// rotate 45 degrees per second
	m_angle(0),
	m_tracking(false),
	m_mappedConstantBuffer(nullptr),
	m_deviceResources(deviceResources),
	m_shouldRotate(false),
	m_supportsSamplerFeedback(false)
{
	ZeroMemory(&m_constantBufferData, sizeof(m_constantBufferData));

	DX::ThrowIfFailed(CoInitialize(nullptr));

	DX::ThrowIfFailed(CoCreateInstance(
		CLSID_WICImagingFactory,
		NULL,
		CLSCTX_INPROC_SERVER,
		IID_IWICImagingFactory,
		(LPVOID*)&m_wicImagingFactory));

	CreateDeviceDependentResources();
	CreateWindowSizeDependentResources();
}

Sample3DSceneRenderer::~Sample3DSceneRenderer()
{
	m_constantBuffer->Unmap(0, nullptr);
	m_mappedConstantBuffer = nullptr;
}

void Sample3DSceneRenderer::CreateDeviceDependentResources()
{
	auto d3dDevice = m_deviceResources->GetD3DDevice();

	D3D12_FEATURE_DATA_D3D12_OPTIONS7 options7{};
	if (SUCCEEDED(d3dDevice->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &options7, sizeof(options7))))
	{
		m_supportsSamplerFeedback = options7.SamplerFeedbackTier > D3D12_SAMPLER_FEEDBACK_TIER_NOT_SUPPORTED;
	}
	
	// Create a root signature with a single constant buffer slot.
	{
		CD3DX12_DESCRIPTOR_RANGE ranges[3];
		CD3DX12_ROOT_PARAMETER parameter;

		ranges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, 0, 0u, 0);
		ranges[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 1);
		ranges[2].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 2);

		parameter.InitAsDescriptorTable(_countof(ranges), ranges, D3D12_SHADER_VISIBILITY_ALL);

		D3D12_ROOT_SIGNATURE_FLAGS rootSignatureFlags =
			D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT | // Only the input assembler stage needs access to the constant buffer.
			D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
			D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS |
			D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS;
		
		D3D12_STATIC_SAMPLER_DESC sampler = {};
		sampler.Filter = D3D12_FILTER_MIN_MAG_POINT_MIP_LINEAR;
		sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
		sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
		sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
		sampler.MipLODBias = 0;
		sampler.MaxAnisotropy = 0;
		sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
		sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
		sampler.MinLOD = 0.0f;
		sampler.MaxLOD = D3D12_FLOAT32_MAX;
		sampler.ShaderRegister = 0;
		sampler.RegisterSpace = 0;
		sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

		CD3DX12_ROOT_SIGNATURE_DESC descRootSignature;
		descRootSignature.Init(1, &parameter, 1, &sampler, rootSignatureFlags);

		ComPtr<ID3DBlob> pSignature;
		ComPtr<ID3DBlob> pError;
		DX::ThrowIfFailed(D3D12SerializeRootSignature(&descRootSignature, D3D_ROOT_SIGNATURE_VERSION_1, pSignature.GetAddressOf(), pError.GetAddressOf()));
		DX::ThrowIfFailed(d3dDevice->CreateRootSignature(0, pSignature->GetBufferPointer(), pSignature->GetBufferSize(), IID_PPV_ARGS(&m_rootSignature)));
        NAME_D3D12_OBJECT(m_rootSignature);
	}

	// Create the pipeline state once the shaders are loaded.
	{

		static const D3D12_INPUT_ELEMENT_DESC inputLayout[] =
		{
			{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
			{ "COLOR", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
			{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
		};

		D3D12_GRAPHICS_PIPELINE_STATE_DESC state = {};
		state.InputLayout = { inputLayout, _countof(inputLayout) };
		state.pRootSignature = m_rootSignature.Get();
        state.VS = CD3DX12_SHADER_BYTECODE((void*)(g_SampleVertexShader), _countof(g_SampleVertexShader));
        state.PS = CD3DX12_SHADER_BYTECODE((void*)(g_SamplePixelShader), _countof(g_SamplePixelShader));
		state.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
		state.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
		state.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
		state.SampleMask = UINT_MAX;
		state.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		state.NumRenderTargets = 1;
		state.RTVFormats[0] = m_deviceResources->GetBackBufferFormat();
		state.DSVFormat = m_deviceResources->GetDepthBufferFormat();
		state.SampleDesc.Count = 1;

		DX::ThrowIfFailed(m_deviceResources->GetD3DDevice()->CreateGraphicsPipelineState(&state, IID_PPV_ARGS(&m_pipelineState)));
	};

	// Create and upload cube geometry resources to the GPU.
	{
		auto d3dDevice = m_deviceResources->GetD3DDevice();

		// Create a command list.
		DX::ThrowIfFailed(d3dDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_deviceResources->GetCommandAllocator(), m_pipelineState.Get(), IID_PPV_ARGS(&m_commandList)));
        NAME_D3D12_OBJECT(m_commandList);

		// Cube vertices. Each vertex has a position and a color.
		VertexPositionColorTex cubeVertices[] =
		{
			// Front face
			{ XMFLOAT3(-0.5f,  0.5f,  0.5f), XMFLOAT3(0.0f, 1.0f, 1.0f), XMFLOAT2(0.0f, 0.0f) }, // fg top left
			{ XMFLOAT3(0.5f,  0.5f,  0.5f), XMFLOAT3(1.0f, 1.0f, 1.0f), XMFLOAT2(1.0f, 0.0f) },  // fg top right
			{ XMFLOAT3(-0.5f, -0.5f,  0.5f), XMFLOAT3(0.0f, 0.0f, 1.0f), XMFLOAT2(0.0f, 1.0f) }, // fg bottom left
			{ XMFLOAT3(0.5f, -0.5f,  0.5f), XMFLOAT3(1.0f, 0.0f, 1.0f), XMFLOAT2(1.0f, 1.0f) },	 // fg bottom right

			// Right face
			{ XMFLOAT3(0.5f,  0.5f,  0.5f), XMFLOAT3(1.0f, 1.0f, 1.0f), XMFLOAT2(0.0f, 0.0f) },  // fg top right
			{ XMFLOAT3(0.5f,  0.5f, -0.5f), XMFLOAT3(1.0f, 1.0f, 0.0f), XMFLOAT2(1.0f, 0.0f) }, // bg top right
			{ XMFLOAT3(0.5f, -0.5f,  0.5f), XMFLOAT3(1.0f, 0.0f, 1.0f), XMFLOAT2(0.0f, 1.0f) },	 // fg bottom right
			{ XMFLOAT3(0.5f, -0.5f, -0.5f), XMFLOAT3(1.0f, 0.0f, 0.0f), XMFLOAT2(1.0f, 1.0f) }, // bg bottom right

			// Back face
			{ XMFLOAT3(0.5f,  0.5f, -0.5f), XMFLOAT3(1.0f, 1.0f, 0.0f), XMFLOAT2(0.0f, 0.0f) }, // bg top right
			{ XMFLOAT3(-0.5f,  0.5f, -0.5f), XMFLOAT3(0.0f, 1.0f, 0.0f), XMFLOAT2(1.0f, 0.0f) }, // bg top left
			{ XMFLOAT3(0.5f, -0.5f, -0.5f), XMFLOAT3(1.0f, 0.0f, 0.0f), XMFLOAT2(0.0f, 1.0f) }, // bg bottom right
			{ XMFLOAT3(-0.5f, -0.5f, -0.5f), XMFLOAT3(0.0f, 0.0f, 0.0f), XMFLOAT2(1.0f, 1.0f) }, // bg bottom left

			// Left face
			{ XMFLOAT3(-0.5f,  0.5f, -0.5f), XMFLOAT3(0.0f, 1.0f, 0.0f), XMFLOAT2(0.0f, 0.0f) }, // bg top left
			{ XMFLOAT3(-0.5f,  0.5f,  0.5f), XMFLOAT3(0.0f, 1.0f, 1.0f), XMFLOAT2(1.0f, 0.0f) }, // fg top left
			{ XMFLOAT3(-0.5f, -0.5f, -0.5f), XMFLOAT3(0.0f, 0.0f, 0.0f), XMFLOAT2(0.0f, 1.0f) }, // bg bottom left
			{ XMFLOAT3(-0.5f, -0.5f,  0.5f), XMFLOAT3(0.0f, 0.0f, 1.0f), XMFLOAT2(1.0f, 1.0f) }, // fg bottom left

			// Top face
			{ XMFLOAT3(-0.5f,  0.5f, -0.5f), XMFLOAT3(0.0f, 1.0f, 0.0f), XMFLOAT2(0.0f, 0.0f) }, // bg top left
			{ XMFLOAT3(0.5f,  0.5f, -0.5f), XMFLOAT3(1.0f, 1.0f, 0.0f), XMFLOAT2(1.0f, 0.0f) }, // bg top right
			{ XMFLOAT3(-0.5f,  0.5f,  0.5f), XMFLOAT3(0.0f, 1.0f, 1.0f), XMFLOAT2(0.0f, 1.0f) }, // fg top left
			{ XMFLOAT3(0.5f,  0.5f,  0.5f), XMFLOAT3(1.0f, 1.0f, 1.0f), XMFLOAT2(1.0f, 1.0f) },  // fg top right

			// Bottom face
			{ XMFLOAT3(-0.5f, -0.5f,  0.5f), XMFLOAT3(0.0f, 0.0f, 1.0f), XMFLOAT2(0.0f, 0.0f) }, // fg bottom left
			{ XMFLOAT3(0.5f, -0.5f,  0.5f), XMFLOAT3(1.0f, 0.0f, 1.0f), XMFLOAT2(1.0f, 0.0f) },	 // fg bottom right
			{ XMFLOAT3(-0.5f, -0.5f, -0.5f), XMFLOAT3(0.0f, 0.0f, 0.0f), XMFLOAT2(0.0f, 1.0f) }, // bg bottom left
			{ XMFLOAT3(0.5f, -0.5f, -0.5f), XMFLOAT3(1.0f, 0.0f, 0.0f), XMFLOAT2(1.0f, 1.0f) }, // bg bottom right
		};

		const UINT vertexBufferSize = sizeof(cubeVertices);

		// Create the vertex buffer resource in the GPU's default heap and copy vertex data into it using the upload heap.
		// The upload resource must not be released until after the GPU has finished using it.
		Microsoft::WRL::ComPtr<ID3D12Resource> vertexBufferUpload;

		CD3DX12_HEAP_PROPERTIES defaultHeapProperties(D3D12_HEAP_TYPE_DEFAULT);
		CD3DX12_RESOURCE_DESC vertexBufferDesc = CD3DX12_RESOURCE_DESC::Buffer(vertexBufferSize);
		DX::ThrowIfFailed(d3dDevice->CreateCommittedResource(
			&defaultHeapProperties,
			D3D12_HEAP_FLAG_NONE,
			&vertexBufferDesc,
			D3D12_RESOURCE_STATE_COPY_DEST,
			nullptr,
			IID_PPV_ARGS(&m_vertexBuffer)));

		CD3DX12_HEAP_PROPERTIES uploadHeapProperties(D3D12_HEAP_TYPE_UPLOAD);
		DX::ThrowIfFailed(d3dDevice->CreateCommittedResource(
			&uploadHeapProperties,
			D3D12_HEAP_FLAG_NONE,
			&vertexBufferDesc,
			D3D12_RESOURCE_STATE_GENERIC_READ,
			nullptr,
			IID_PPV_ARGS(&vertexBufferUpload)));

        NAME_D3D12_OBJECT(m_vertexBuffer);

		// Upload the vertex buffer to the GPU.
		{
			D3D12_SUBRESOURCE_DATA vertexData = {};
			vertexData.pData = reinterpret_cast<BYTE*>(cubeVertices);
			vertexData.RowPitch = vertexBufferSize;
			vertexData.SlicePitch = vertexData.RowPitch;

			UpdateSubresources(m_commandList.Get(), m_vertexBuffer.Get(), vertexBufferUpload.Get(), 0, 0, 1, &vertexData);

			CD3DX12_RESOURCE_BARRIER vertexBufferResourceBarrier =
				CD3DX12_RESOURCE_BARRIER::Transition(m_vertexBuffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
			m_commandList->ResourceBarrier(1, &vertexBufferResourceBarrier);
		}

		// Load mesh indices. Each trio of indices represents a triangle to be rendered on the screen.
		// For example: 0,2,1 means that the vertices with indexes 0, 2 and 1 from the vertex buffer compose the
		// first triangle of this mesh.

		std::vector<unsigned short> cubeIndices;

		unsigned short baseIndex = 0;
		for (int i = 0; i < 6; ++i)
		{
			cubeIndices.push_back(baseIndex + 0);
			cubeIndices.push_back(baseIndex + 1);
			cubeIndices.push_back(baseIndex + 2);

			cubeIndices.push_back(baseIndex + 2);
			cubeIndices.push_back(baseIndex + 1);
			cubeIndices.push_back(baseIndex + 3);

			baseIndex += 4;
		}

		m_indexCount = static_cast<UINT>(cubeIndices.size());

		const UINT indexBufferSize = m_indexCount * sizeof(unsigned short);

		// Create the index buffer resource in the GPU's default heap and copy index data into it using the upload heap.
		// The upload resource must not be released until after the GPU has finished using it.
		Microsoft::WRL::ComPtr<ID3D12Resource> indexBufferUpload;

		CD3DX12_RESOURCE_DESC indexBufferDesc = CD3DX12_RESOURCE_DESC::Buffer(indexBufferSize);
		DX::ThrowIfFailed(d3dDevice->CreateCommittedResource(
			&defaultHeapProperties,
			D3D12_HEAP_FLAG_NONE,
			&indexBufferDesc,
			D3D12_RESOURCE_STATE_COPY_DEST,
			nullptr,
			IID_PPV_ARGS(&m_indexBuffer)));

		DX::ThrowIfFailed(d3dDevice->CreateCommittedResource(
			&uploadHeapProperties,
			D3D12_HEAP_FLAG_NONE,
			&indexBufferDesc,
			D3D12_RESOURCE_STATE_GENERIC_READ,
			nullptr,
			IID_PPV_ARGS(&indexBufferUpload)));

		NAME_D3D12_OBJECT(m_indexBuffer);

		// Upload the index buffer to the GPU.
		{
			D3D12_SUBRESOURCE_DATA indexData = {};
			indexData.pData = reinterpret_cast<BYTE*>(cubeIndices.data());
			indexData.RowPitch = indexBufferSize;
			indexData.SlicePitch = indexData.RowPitch;

			UpdateSubresources(m_commandList.Get(), m_indexBuffer.Get(), indexBufferUpload.Get(), 0, 0, 1, &indexData);

			CD3DX12_RESOURCE_BARRIER indexBufferResourceBarrier =
				CD3DX12_RESOURCE_BARRIER::Transition(m_indexBuffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_INDEX_BUFFER);
			m_commandList->ResourceBarrier(1, &indexBufferResourceBarrier);
		}

		// Create a descriptor heap for the constant buffers.
		{
			D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
			
			heapDesc.NumDescriptors = 3;
			// 0 - Constant buffer
			// 1 - SRV
			// 2 - Sampler feedback UAV

			heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
			// This flag indicates that this descriptor heap can be bound to the pipeline and that descriptors contained in it can be referenced by a root table.
			heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
			DX::ThrowIfFailed(d3dDevice->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_cbvSrvUavHeap)));

            NAME_D3D12_OBJECT(m_cbvSrvUavHeap); 
		}
		
		CD3DX12_RESOURCE_DESC constantBufferDesc = CD3DX12_RESOURCE_DESC::Buffer(DX::c_frameCount * c_alignedConstantBufferSize);
		DX::ThrowIfFailed(d3dDevice->CreateCommittedResource(
			&uploadHeapProperties,
			D3D12_HEAP_FLAG_NONE,
			&constantBufferDesc,
			D3D12_RESOURCE_STATE_GENERIC_READ,
			nullptr,
			IID_PPV_ARGS(&m_constantBuffer)));

        NAME_D3D12_OBJECT(m_constantBuffer);

		// Create constant buffer views to access the upload buffer.
		D3D12_GPU_VIRTUAL_ADDRESS cbvGpuAddress = m_constantBuffer->GetGPUVirtualAddress();
		CD3DX12_CPU_DESCRIPTOR_HANDLE cpuHandle(m_cbvSrvUavHeap->GetCPUDescriptorHandleForHeapStart(), 0, m_cbvSrvUavDescriptorSize);
		m_cbvSrvUavDescriptorSize = d3dDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

		D3D12_CONSTANT_BUFFER_VIEW_DESC desc;
		desc.BufferLocation = cbvGpuAddress;
		desc.SizeInBytes = c_alignedConstantBufferSize;
		d3dDevice->CreateConstantBufferView(&desc, cpuHandle);

		// Map the constant buffer.
		CD3DX12_RANGE readRange(0, 0);		// We do not intend to read from this resource on the CPU.
		DX::ThrowIfFailed(m_constantBuffer->Map(0, &readRange, reinterpret_cast<void**>(&m_mappedConstantBuffer)));
		ZeroMemory(m_mappedConstantBuffer, c_alignedConstantBufferSize);
		// We don't unmap this until the app closes. Keeping things mapped for the lifetime of the resource is okay.

		// Load image resource
		std::vector<std::wstring> imageFileNames;
		imageFileNames.push_back(L"1.png");
		imageFileNames.push_back(L"2.png");
		imageFileNames.push_back(L"3.png");
		imageFileNames.push_back(L"4.png");
		imageFileNames.push_back(L"5.png");
		imageFileNames.push_back(L"6.png");
		LoadTextureFromPngFile(imageFileNames);

		// Close the command list and execute it to begin the vertex/index buffer copy into the GPU's default heap.
		DX::ThrowIfFailed(m_commandList->Close());
		ID3D12CommandList* ppCommandLists[] = { m_commandList.Get() };
		m_deviceResources->GetCommandQueue()->ExecuteCommandLists(_countof(ppCommandLists), ppCommandLists);

		// Create vertex/index buffer views.
		m_vertexBufferView.BufferLocation = m_vertexBuffer->GetGPUVirtualAddress();
		m_vertexBufferView.StrideInBytes = sizeof(VertexPositionColorTex);
		m_vertexBufferView.SizeInBytes = sizeof(cubeVertices);

		m_indexBufferView.BufferLocation = m_indexBuffer->GetGPUVirtualAddress();
		m_indexBufferView.SizeInBytes = indexBufferSize;
		m_indexBufferView.Format = DXGI_FORMAT_R16_UINT;

		// Wait for the command list to finish executing; the vertex/index buffers need to be uploaded to the GPU before the upload resources go out of scope.
		m_deviceResources->WaitForGpu();

		// Create feedback map
		if (m_supportsSamplerFeedback)
		{
			D3D12_RESOURCE_DESC pairedTextureDesc = m_texture->GetDesc();

			CD3DX12_RESOURCE_DESC1 feedbackTextureDesc = CD3DX12_RESOURCE_DESC1::Tex2D(
				DXGI_FORMAT_SAMPLER_FEEDBACK_MIN_MIP_OPAQUE,
				pairedTextureDesc.Width,
				pairedTextureDesc.Height,
				pairedTextureDesc.DepthOrArraySize,
				pairedTextureDesc.MipLevels,
				1, // sample count
				0, // sample quality
				D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
				D3D12_TEXTURE_LAYOUT_UNKNOWN,
				65536,
				16, 16, 1);

			DX::ThrowIfFailed(d3dDevice->CreateCommittedResource2(
				&defaultHeapProperties,
				D3D12_HEAP_FLAG_NONE,
				&feedbackTextureDesc,
				D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
				nullptr,
				nullptr,
				IID_PPV_ARGS(&m_feedbackTexture)));

			CD3DX12_CPU_DESCRIPTOR_HANDLE cpuHandle(m_cbvSrvUavHeap->GetCPUDescriptorHandleForHeapStart(), 2, m_cbvSrvUavDescriptorSize);

			d3dDevice->CreateSamplerFeedbackUnorderedAccessView(m_texture.Get(), m_feedbackTexture.Get(), cpuHandle);

			CD3DX12_HEAP_PROPERTIES readbackHeapProperties(D3D12_HEAP_TYPE_READBACK);
			
			UINT requiredWidth = static_cast<UINT>(feedbackTextureDesc.Width) / feedbackTextureDesc.SamplerFeedbackMipRegion.Width;
			UINT requiredHeight = feedbackTextureDesc.Height / feedbackTextureDesc.SamplerFeedbackMipRegion.Height;

			CD3DX12_RESOURCE_DESC r8TextureDesc = CD3DX12_RESOURCE_DESC::Tex2D(
				DXGI_FORMAT_R8_UINT,
				requiredWidth,
				requiredHeight,
				1,
				1 /* mip count */);

			DX::ThrowIfFailed(d3dDevice->CreateCommittedResource(
				&defaultHeapProperties,
				D3D12_HEAP_FLAG_NONE,
				&r8TextureDesc,
				D3D12_RESOURCE_STATE_RESOLVE_DEST,
				nullptr,
				IID_PPV_ARGS(&m_decodeTexture)));

			m_decodeTextureLayouts.resize(r8TextureDesc.DepthOrArraySize);
			UINT64 r8TextureTotalBytes{};
			d3dDevice->GetCopyableFootprints(
				&r8TextureDesc,
				0,
				r8TextureDesc.DepthOrArraySize,
				0,
				m_decodeTextureLayouts.data(),
				nullptr,
				nullptr,
				&r8TextureTotalBytes);

			CD3DX12_RESOURCE_DESC readbackResourceDesc = CD3DX12_RESOURCE_DESC::Buffer(r8TextureTotalBytes);
			
			DX::ThrowIfFailed(d3dDevice->CreateCommittedResource(
				&readbackHeapProperties,
				D3D12_HEAP_FLAG_NONE,
				&readbackResourceDesc,
				D3D12_RESOURCE_STATE_COPY_DEST,
				nullptr,
				IID_PPV_ARGS(&m_decodeBuffer)));

			DX::ThrowIfFailed(m_decodeBuffer->Map(0, nullptr, reinterpret_cast<void**>(&m_decodeBufferMapped)));
		}

		D2D1_FACTORY_OPTIONS factoryOptions{};
#if defined(_DEBUG)
		factoryOptions.debugLevel = D2D1_DEBUG_LEVEL_INFORMATION;
#endif
		DX::ThrowIfFailed(D2D1CreateFactory<ID2D1Factory1>(D2D1_FACTORY_TYPE_SINGLE_THREADED, factoryOptions, &m_d2dFactory));
			   
		uint32_t d3d11DeviceFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_PREVENT_INTERNAL_THREADING_OPTIMIZATIONS;
#if defined(_DEBUG)
		d3d11DeviceFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
		IUnknown* commandQueues[] = { m_deviceResources->GetCommandQueue() };
		DX::ThrowIfFailed(D3D11On12CreateDevice(
			m_deviceResources->GetD3DDevice(),
			d3d11DeviceFlags,
			nullptr,
			0,
			commandQueues,
			1,
			0,
			&m_device11,
			&m_deviceContext11,
			nullptr));

		DX::ThrowIfFailed(m_device11.As(&m_device11on12));

		ComPtr<IDXGIDevice> dxgiDevice;
		DX::ThrowIfFailed(m_device11.As(&dxgiDevice));

		DX::ThrowIfFailed(m_d2dFactory->CreateDevice(dxgiDevice.Get(), &m_d2dDevice));
		D2D1_DEVICE_CONTEXT_OPTIONS deviceOptions = D2D1_DEVICE_CONTEXT_OPTIONS_NONE;
		DX::ThrowIfFailed(m_d2dDevice->CreateDeviceContext(deviceOptions, &m_d2dDeviceContext));

		IDXGISwapChain* swapChain = m_deviceResources->GetSwapChain();

		DXGI_SWAP_CHAIN_DESC swapChainDesc;
		DX::ThrowIfFailed(swapChain->GetDesc(&swapChainDesc));

		for (int i = 0; i < swapChainDesc.BufferCount; ++i)
		{
			PerBackBuffer2DResource r;

			ComPtr<ID3D12Resource> backbuffer;
			DX::ThrowIfFailed(swapChain->GetBuffer(i, IID_PPV_ARGS(&backbuffer)));

			D3D11_RESOURCE_FLAGS d3d11Flags = { D3D11_BIND_RENDER_TARGET };
			DX::ThrowIfFailed(m_device11on12->CreateWrappedResource(backbuffer.Get(), &d3d11Flags, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_PRESENT, IID_PPV_ARGS(&r.Backbuffer11)));

			ComPtr<IDXGISurface> backBufferSurface;
			DX::ThrowIfFailed(r.Backbuffer11.As(&backBufferSurface));

			D2D1_BITMAP_PROPERTIES1 bitmapProperties = D2D1::BitmapProperties1(
				D2D1_BITMAP_OPTIONS_CANNOT_DRAW | D2D1_BITMAP_OPTIONS_TARGET,
				D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
			DX::ThrowIfFailed(m_d2dDeviceContext->CreateBitmapFromDxgiSurface(backBufferSurface.Get(), bitmapProperties, &r.TargetBitmap));

			m_perBackBuffer2DResources.push_back(r);
		}

		DX::ThrowIfFailed(m_d2dDeviceContext->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Black), &m_d2dBlackBrush));
		DX::ThrowIfFailed(m_d2dDeviceContext->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Magenta), &m_d2dMagentaBrush));
		DX::ThrowIfFailed(m_d2dDeviceContext->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &m_d2dWhiteBrush));

	};

	m_loadingComplete = true;
}

// Initializes view parameters when the window size changes.
void Sample3DSceneRenderer::CreateWindowSizeDependentResources()
{
	DX::SizeU outputSize = m_deviceResources->GetOutputSize();
	float aspectRatio = static_cast<float>(outputSize.Width) / static_cast<float>(outputSize.Height);
	float fovAngleY = 70.0f * XM_PI / 180.0f;

	D3D12_VIEWPORT viewport = m_deviceResources->GetScreenViewport();
	m_scissorRect = { 0, 0, static_cast<LONG>(viewport.Width), static_cast<LONG>(viewport.Height)};

	// This is a simple example of change that can be made when the app is in
	// portrait or snapped view.
	if (aspectRatio < 1.0f)
	{
		fovAngleY *= 2.0f;
	}
	
	// This sample makes use of a right-handed coordinate system using row-major matrices.
	XMMATRIX perspectiveMatrix = XMMatrixPerspectiveFovRH(
		fovAngleY,
		aspectRatio,
		0.01f,
		100.0f
		);

	XMStoreFloat4x4(
		&m_constantBufferData.projection,
		XMMatrixTranspose(perspectiveMatrix)
		);

	// Eye is at (0,0.7,1.5), looking at point (0,-0.1,0) with the up-vector along the y-axis.
	static const XMVECTORF32 eye = { 0.0f, 0.7f, 1.5f, 0.0f };
	static const XMVECTORF32 at = { 0.0f, -0.1f, 0.0f, 0.0f };
	static const XMVECTORF32 up = { 0.0f, 1.0f, 0.0f, 0.0f };

	XMStoreFloat4x4(&m_constantBufferData.view, XMMatrixTranspose(XMMatrixLookAtRH(eye, at, up)));
}

// Called once per frame, rotates the cube and calculates the model and view matrices.
void Sample3DSceneRenderer::Update(DX::StepTimer const& timer)
{
	if (m_loadingComplete)
	{
		if (!m_tracking)
		{
			// Rotate the cube a small amount.
			if (m_shouldRotate)
			{
				m_angle += static_cast<float>(timer.GetElapsedSeconds())* m_radiansPerSecond;
			}

			Rotate(m_angle);
		}

		// Update the constant buffer resource.
		UINT8* destination = m_mappedConstantBuffer;
		memcpy(destination, &m_constantBufferData, sizeof(m_constantBufferData));
	}
}

// Rotate the 3D cube model a set amount of radians.
void Sample3DSceneRenderer::Rotate(float radians)
{
	// Prepare to pass the updated model matrix to the shader.
	XMStoreFloat4x4(&m_constantBufferData.model, XMMatrixTranspose(XMMatrixRotationY(radians)));
}

// Renders one frame using the vertex and pixel shaders.
bool Sample3DSceneRenderer::Render()
{
	// Loading is asynchronous. Only draw geometry after it's loaded.
	if (!m_loadingComplete)
	{
		return false;
	}

	DX::ThrowIfFailed(m_deviceResources->GetCommandAllocator()->Reset());

	// The command list can be reset anytime after ExecuteCommandList() is called.
	DX::ThrowIfFailed(m_commandList->Reset(m_deviceResources->GetCommandAllocator(), m_pipelineState.Get()));

	{
		// Set the graphics root signature and descriptor heaps to be used by this frame.
		m_commandList->SetGraphicsRootSignature(m_rootSignature.Get());
		ID3D12DescriptorHeap* ppHeaps[] = { m_cbvSrvUavHeap.Get() };
		m_commandList->SetDescriptorHeaps(_countof(ppHeaps), ppHeaps);

		// Bind the current frame's constant buffer to the pipeline.
		CD3DX12_GPU_DESCRIPTOR_HANDLE gpuHandle(m_cbvSrvUavHeap->GetGPUDescriptorHandleForHeapStart());
		m_commandList->SetGraphicsRootDescriptorTable(0, gpuHandle);

		// Set the viewport and scissor rectangle.
		D3D12_VIEWPORT viewport = m_deviceResources->GetScreenViewport();
		m_commandList->RSSetViewports(1, &viewport);
		m_commandList->RSSetScissorRects(1, &m_scissorRect);

		// Indicate this resource will be in use as a render target.
		CD3DX12_RESOURCE_BARRIER renderTargetResourceBarrier =
			CD3DX12_RESOURCE_BARRIER::Transition(m_deviceResources->GetRenderTarget(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
		m_commandList->ResourceBarrier(1, &renderTargetResourceBarrier);

		// Record drawing commands.
		D3D12_CPU_DESCRIPTOR_HANDLE renderTargetView = m_deviceResources->GetRenderTargetView();
		D3D12_CPU_DESCRIPTOR_HANDLE depthStencilView = m_deviceResources->GetDepthStencilView();

		float cornflowerBlue[] = {0.3f, 0.58f, 0.93f, 1.0f};
		m_commandList->ClearRenderTargetView(renderTargetView, cornflowerBlue, 0, nullptr);
		m_commandList->ClearDepthStencilView(depthStencilView, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

		m_commandList->OMSetRenderTargets(1, &renderTargetView, false, &depthStencilView);

		m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		m_commandList->IASetVertexBuffers(0, 1, &m_vertexBufferView);
		m_commandList->IASetIndexBuffer(&m_indexBufferView);
		m_commandList->DrawIndexedInstanced(m_indexCount, 1, 0, 0, 0);

		// Indicate that the render target will now be used to present when the command list is done executing.
		CD3DX12_RESOURCE_BARRIER presentResourceBarrier =
			CD3DX12_RESOURCE_BARRIER::Transition(m_deviceResources->GetRenderTarget(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
		m_commandList->ResourceBarrier(1, &presentResourceBarrier);


		CD3DX12_RESOURCE_BARRIER feedbackTransition0 =
			CD3DX12_RESOURCE_BARRIER::Transition(m_feedbackTexture.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
		m_commandList->ResourceBarrier(1, &feedbackTransition0);

		m_commandList->ResolveSubresourceRegion(
			m_decodeTexture.Get(), 0, 0, 0, m_feedbackTexture.Get(), 0, nullptr, DXGI_FORMAT_R8_UINT, D3D12_RESOLVE_MODE_DECODE_SAMPLER_FEEDBACK);

		CD3DX12_RESOURCE_BARRIER feedbackTransition1 =
			CD3DX12_RESOURCE_BARRIER::Transition(m_feedbackTexture.Get(), D3D12_RESOURCE_STATE_RESOLVE_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		m_commandList->ResourceBarrier(1, &feedbackTransition1);

		CD3DX12_RESOURCE_BARRIER decodeTransition0 =
			CD3DX12_RESOURCE_BARRIER::Transition(m_decodeTexture.Get(), D3D12_RESOURCE_STATE_RESOLVE_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
		m_commandList->ResourceBarrier(1, &decodeTransition0);

		// Get data from decodeTexture
		D3D12_TEXTURE_COPY_LOCATION dst{};
		dst.pResource = m_decodeBuffer.Get();
		dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
		dst.PlacedFootprint.Footprint = m_decodeTextureLayouts[0].Footprint;

		D3D12_TEXTURE_COPY_LOCATION src{};
		src.pResource = m_decodeTexture.Get();
		src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		src.SubresourceIndex = 0;

		m_commandList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

		CD3DX12_RESOURCE_BARRIER decodeTransition1 =
			CD3DX12_RESOURCE_BARRIER::Transition(m_decodeTexture.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RESOLVE_DEST);
		m_commandList->ResourceBarrier(1, &decodeTransition1);
	}

	DX::ThrowIfFailed(m_commandList->Close());

	// Execute the command list.
	ID3D12CommandList* ppCommandLists[] = { m_commandList.Get() };
	m_deviceResources->GetCommandQueue()->ExecuteCommandLists(_countof(ppCommandLists), ppCommandLists);	
	   
	{
		UINT frameIndex = m_deviceResources->GetCurrentFrameIndex();
		auto const& backbuffer2D = m_perBackBuffer2DResources[frameIndex];

		float gridScreenSize = 400;

		ID3D11Resource* wrappedResources[] = { backbuffer2D.Backbuffer11.Get() };
		m_device11on12->AcquireWrappedResources(wrappedResources, ARRAYSIZE(wrappedResources));
		m_d2dDeviceContext->SetTarget(backbuffer2D.TargetBitmap.Get());
		m_d2dDeviceContext->BeginDraw();
		m_d2dDeviceContext->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
		m_d2dDeviceContext->FillRectangle(D2D1::RectF(0, 0, gridScreenSize, gridScreenSize), m_d2dWhiteBrush.Get());

		D3D12_RESOURCE_DESC decodeTextureDesc = m_decodeTexture->GetDesc();
		float gridWidth = decodeTextureDesc.Width;
		float gridHeight = decodeTextureDesc.Height;

		float gridScreenCellWidth = gridScreenSize / gridWidth;
		float gridScreenCellHeight = gridScreenSize / gridHeight;

		float screenY = 0;
		for (int y = 0; y < gridHeight; y++)
		{
			byte* mappedRowStart = m_decodeBufferMapped + (m_decodeTextureLayouts[0].Footprint.RowPitch * y);
			
			float screenX = 0;
			for (int x = 0; x < gridWidth; ++x)
			{
				D2D1_RECT_F rect = D2D1::RectF(screenX, screenY, screenX + gridScreenCellWidth, screenY + gridScreenCellHeight);
				
				m_decodeTextureLayouts[0].Footprint.RowPitch;
				
				byte mipLevel = mappedRowStart[x];
				D2D1::ColorF::Enum fillColorKey;
				switch (mipLevel)
				{
					case 0x00:fillColorKey = D2D1::ColorF::Maroon; break;
					case 0x01:fillColorKey = D2D1::ColorF::Orange; break;
					case 0x02:fillColorKey = D2D1::ColorF::Green; break;
					case 0x03:fillColorKey = D2D1::ColorF::Purple; break;
					case 0x04:fillColorKey = D2D1::ColorF::Gray; break;
					case 0x05:fillColorKey = D2D1::ColorF::LightCyan; break;
					case 0xFF: fillColorKey = D2D1::ColorF::Black; break;
					default: fillColorKey = D2D1::ColorF::Magenta; break;
				}
				m_d2dMagentaBrush->SetColor(D2D1::ColorF(fillColorKey));
				m_d2dDeviceContext->FillRectangle(rect, m_d2dMagentaBrush.Get());

				m_d2dDeviceContext->DrawRectangle(rect, m_d2dBlackBrush.Get());
				screenX += gridScreenCellWidth;
			}
			screenY += gridScreenCellHeight;
		}

		DX::ThrowIfFailed(m_d2dDeviceContext->EndDraw());

		m_device11on12->ReleaseWrappedResources(wrappedResources, ARRAYSIZE(wrappedResources));

	}

	m_deviceContext11->Flush(); // Submits the 11on12 layering's 12 command lists

	///////////

	return true;
}

Sample3DSceneRenderer::LoadedImageData Sample3DSceneRenderer::LoadImageDataFromPngFile(std::wstring fileName)
{
	LoadedImageData result{};

	ComPtr<IWICBitmapDecoder> decoder;
	DX::ThrowIfFailed(m_wicImagingFactory->CreateDecoderFromFilename(
		fileName.c_str(),
		NULL,
		GENERIC_READ,
		WICDecodeMetadataCacheOnLoad, &decoder));

	ComPtr<IWICBitmapFrameDecode> spSource;
	DX::ThrowIfFailed(decoder->GetFrame(0, &spSource));

	ComPtr<IWICFormatConverter> spConverter;
	DX::ThrowIfFailed(m_wicImagingFactory->CreateFormatConverter(&spConverter));

	DX::ThrowIfFailed(spConverter->Initialize(
		spSource.Get(),
		GUID_WICPixelFormat32bppPBGRA,
		WICBitmapDitherTypeNone,
		NULL,
		0.f,
		WICBitmapPaletteTypeMedianCut));

	DX::ThrowIfFailed(spConverter->GetSize(&result.ImageWidth, &result.ImageHeight));

	result.Buffer.resize(result.ImageWidth * result.ImageHeight);
	DX::ThrowIfFailed(spConverter->CopyPixels(
		NULL,
		result.ImageWidth * sizeof(UINT),
		static_cast<UINT>(result.Buffer.size()) * sizeof(UINT),
		reinterpret_cast<BYTE*>(result.Buffer.data())));

	return result;
}

void Sample3DSceneRenderer::LoadTextureFromPngFile(std::vector<std::wstring> const& mipImageFileNames)
{
	// Precondition: images are of the correct size, from largest to smallest.

	std::vector<LoadedImageData> loadedImageDatas;
	for (size_t i = 0; i < mipImageFileNames.size(); ++i)
	{
		loadedImageDatas.push_back(LoadImageDataFromPngFile(mipImageFileNames[i]));
	}

	D3D12_RESOURCE_DESC resourceDesc{};
	resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	resourceDesc.Width = loadedImageDatas[0].ImageWidth;
	resourceDesc.Height = loadedImageDatas[0].ImageHeight;
	resourceDesc.MipLevels = static_cast<UINT>(loadedImageDatas.size());
	resourceDesc.DepthOrArraySize = 1;
	resourceDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	resourceDesc.SampleDesc.Count = 1;
	resourceDesc.SampleDesc.Quality = 0;
	
	DX::ThrowIfFailed(m_deviceResources->GetD3DDevice()->CreateCommittedResource(
		&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
		D3D12_HEAP_FLAG_NONE,
		&resourceDesc,
		D3D12_RESOURCE_STATE_COPY_DEST,
		nullptr,
		IID_PPV_ARGS(&m_texture)));

	CD3DX12_CPU_DESCRIPTOR_HANDLE cpuHandle(m_cbvSrvUavHeap->GetCPUDescriptorHandleForHeapStart(), 1, m_cbvSrvUavDescriptorSize);
	
	// Describe and create a SRV for the texture.
	D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
	srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srvDesc.Format = resourceDesc.Format;
	srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	srvDesc.Texture2D.MipLevels = resourceDesc.MipLevels;
	m_deviceResources->GetD3DDevice()->CreateShaderResourceView(m_texture.Get(), &srvDesc, cpuHandle);

	for (int currentMipLevel = 0; currentMipLevel < loadedImageDatas.size(); ++currentMipLevel)
	{
		const UINT64 uploadBufferSize = GetRequiredIntermediateSize(m_texture.Get(), currentMipLevel, 1);

		ComPtr<ID3D12Resource> upload;
		DX::ThrowIfFailed(m_deviceResources->GetD3DDevice()->CreateCommittedResource(
			&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
			D3D12_HEAP_FLAG_NONE,
			&CD3DX12_RESOURCE_DESC::Buffer(uploadBufferSize),
			D3D12_RESOURCE_STATE_GENERIC_READ,
			nullptr,
			IID_PPV_ARGS(&upload)));

		D3D12_SUBRESOURCE_DATA initialData{};
		initialData.pData = loadedImageDatas[currentMipLevel].Buffer.data();
		initialData.RowPitch = loadedImageDatas[currentMipLevel].ImageWidth * 4;
		initialData.SlicePitch = loadedImageDatas[currentMipLevel].ImageWidth * loadedImageDatas[currentMipLevel].ImageHeight * 4;
		UpdateSubresources(m_commandList.Get(), m_texture.Get(), upload.Get(), 0, currentMipLevel, 1, &initialData);

		m_uploads.push_back(upload);
	}

	CD3DX12_RESOURCE_BARRIER resourceBarrier =
		CD3DX12_RESOURCE_BARRIER::Transition(m_texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	m_commandList->ResourceBarrier(1, &resourceBarrier);
}

void Sample3DSceneRenderer::OnKeyUp(WPARAM wParam)
{
	if (wParam == 32)
	{
		// Toggle rotation
		m_shouldRotate = !m_shouldRotate;
	}
}