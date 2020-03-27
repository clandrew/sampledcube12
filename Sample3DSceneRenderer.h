#pragma once

#include "Common\DeviceResources.h"
#include "ShaderStructures.h"
#include "Common\StepTimer.h"

using namespace Microsoft::WRL;

namespace SpinningCube
{
	// This sample renderer instantiates a basic rendering pipeline.
	class Sample3DSceneRenderer
	{
	public:
		Sample3DSceneRenderer(const std::shared_ptr<DX::DeviceResources>& deviceResources);
		~Sample3DSceneRenderer();
		void CreateDeviceDependentResources();
		void CreateWindowSizeDependentResources();
		void Update(DX::StepTimer const& timer);
		bool Render();
		void OnKeyUp(WPARAM wParam);

	private:
		void Rotate(float radians);

		struct LoadedImageData
		{
			UINT ImageWidth;
			UINT ImageHeight;
			std::vector<UINT> Buffer;
		};
		LoadedImageData LoadImageDataFromPngFile(std::wstring fileName);
		void LoadTextureFromPngFile(std::vector<std::wstring> const& mipImageFileNames);

	private:
		// Constant buffers must be 256-byte aligned.
		static const UINT c_alignedConstantBufferSize = (sizeof(ModelViewProjectionConstantBuffer) + 255) & ~255;

		// Cached pointer to device resources.
		std::shared_ptr<DX::DeviceResources> m_deviceResources;

		// Direct3D resources for cube geometry.
		ComPtr<ID3D12GraphicsCommandList1>	m_commandList;
		ComPtr<ID3D12RootSignature>			m_rootSignature;
		ComPtr<ID3D12PipelineState>			m_pipelineState;
		ComPtr<ID3D12DescriptorHeap>		m_cbvSrvUavHeap;
		ComPtr<ID3D12Resource>				m_vertexBuffer;
		ComPtr<ID3D12Resource>				m_indexBuffer;
		ComPtr<ID3D12Resource>				m_constantBuffer;
		ModelViewProjectionConstantBuffer	m_constantBufferData;
		UINT8*								m_mappedConstantBuffer;
		UINT								m_cbvSrvUavDescriptorSize;
		D3D12_RECT							m_scissorRect;
		D3D12_VERTEX_BUFFER_VIEW			m_vertexBufferView;
		D3D12_INDEX_BUFFER_VIEW				m_indexBufferView;
		ComPtr<ID3D12Resource>				m_texture;
	    std::vector<ComPtr<ID3D12Resource>> m_uploads;
		UINT								m_indexCount;
		ComPtr<IWICImagingFactory>          m_wicImagingFactory;

		// For sampler feedback
		bool								m_supportsSamplerFeedback;
		byte*								m_decodeBufferMapped;
		ComPtr<ID3D12Resource>				m_feedbackTexture;
		ComPtr<ID3D12Resource>				m_decodeBuffer;
		ComPtr<ID3D12Resource>				m_decodeTexture;		
		std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> m_decodeTextureLayouts;

		// 2D stuff
		ComPtr<ID2D1Factory1>				m_d2dFactory;
		ComPtr<ID3D11Device>				m_device11;
		ComPtr<ID3D11DeviceContext>			m_deviceContext11;
		ComPtr<ID2D1RenderTarget>			m_d2dRenderTarget;
		ComPtr<ID3D11On12Device>			m_device11on12;
		ComPtr<ID2D1Device>					m_d2dDevice;
		ComPtr<ID2D1DeviceContext>			m_d2dDeviceContext;		
		struct PerBackBuffer2DResource
		{
			ComPtr<ID3D11Texture2D>		    Backbuffer11;
			ComPtr<ID2D1Bitmap1>			TargetBitmap;
		};
		std::vector<PerBackBuffer2DResource>	m_perBackBuffer2DResources;
		ComPtr<ID2D1SolidColorBrush>		m_d2dBlackBrush;
		ComPtr<ID2D1SolidColorBrush>		m_d2dWhiteBrush; // For debugging
		ComPtr<ID2D1SolidColorBrush>		m_d2dMagentaBrush; // For debugging

		// Variables used with the rendering loop.
		bool	m_loadingComplete;
		float	m_radiansPerSecond;
		float	m_angle;
		bool	m_tracking;
		bool    m_shouldRotate;
	};
}

