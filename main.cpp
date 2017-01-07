#include <Windows.h>
#include <wrl/client.h>
#include <wincodec.h>
#include <dxgi1_5.h>
#include <dxgidebug.h>
#include <d3d11_4.h>
#include <DirectXMath.h>

#include "generated/VertexShader.h"
#include "generated/PixelShader.h"

#pragma comment(lib, "dxguid")
#pragma comment(lib, "dxgi")
#pragma comment(lib, "d3d11")

#define USE_WARP_DEVICE 0 // set to 1 to use WARP device instead of hardware device

using namespace Microsoft::WRL;
using namespace DirectX;

#undef NULL
#define NULL nullptr

#if _DEBUG
const bool kDebugBuild = true;
#else
const bool kDebugBuild = false;
#endif

const DXGI_FORMAT kFormat = DXGI_FORMAT_B8G8R8A8_UNORM;

LPCWSTR kWindowClass = L"MainWindow";

float mWidth = 0;
float mHeight = 0;

#pragma region error checking helpers

void HandleFatalError(HRESULT hr)
{
	if (SUCCEEDED(hr)) hr = E_UNEXPECTED;
	TerminateProcess(GetCurrentProcess(), hr);
	for (;;) Sleep(INFINITE);
}

HRESULT GetLastErrorHR()
{
	return HRESULT_FROM_WIN32(GetLastError());
}

void FailHR(HRESULT hr)
{
	if (IsDebuggerPresent())
		__debugbreak();

	HandleFatalError(hr);
}

void FailWin32(DWORD error)
{
	FailHR(HRESULT_FROM_WIN32(error));
}

void FailLastWin32()
{
	FailWin32(GetLastError());
}

void Check(bool cond)
{
	if (!cond)
		FailWin32(ERROR_ASSERTION_FAILURE);
}

void CheckHR(HRESULT hr)
{
	if (FAILED(hr))
		FailHR(hr);
}

void CheckLastWin32(bool cond)
{
	if (!cond)
		FailLastWin32();
}

#pragma endregion

struct Globals
{
	ComPtr<IDXGIFactory5> mGraphicsFactory;
	ComPtr<IDXGISwapChain4> mSwapChain;
	ComPtr<ID3D11Device4> mRenderDevice;
	ComPtr<ID3D11DeviceContext3> mRenderContext;
	ComPtr<ID3D11RenderTargetView> mRenderTarget;
	ComPtr<ID3D11Texture2D> mTexture;
	ComPtr<ID3D11Buffer> mTextureBuffer;
	ComPtr<ID3D11Buffer> mVertexBuffer;
}*g;

void InitializeDevice(HWND hwnd)
{
	if (kDebugBuild)
	{
		ComPtr<IDXGIInfoQueue> pQueue;
		CheckHR(DXGIGetDebugInterface1(0, IID_PPV_ARGS(&pQueue)));
		CheckHR(pQueue->SetBreakOnSeverity(DXGI_DEBUG_ALL, DXGI_INFO_QUEUE_MESSAGE_SEVERITY_CORRUPTION, TRUE));
		CheckHR(pQueue->SetBreakOnSeverity(DXGI_DEBUG_ALL, DXGI_INFO_QUEUE_MESSAGE_SEVERITY_ERROR, TRUE));
	}

	{
		UINT flags = 0;
		if (kDebugBuild) flags |= DXGI_CREATE_FACTORY_DEBUG;
		CheckHR(CreateDXGIFactory2(flags, IID_PPV_ARGS(&g->mGraphicsFactory)));
	}

	{
		D3D_DRIVER_TYPE type = D3D_DRIVER_TYPE_HARDWARE;
#if USE_WARP_DEVICE
		type = D3D_DRIVER_TYPE_WARP;
#endif
		UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
		if (kDebugBuild) flags |= D3D11_CREATE_DEVICE_DEBUG;
		D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_11_1;
		ComPtr<ID3D11Device> pDevice;
		ComPtr<ID3D11DeviceContext> pContext;
		CheckHR(D3D11CreateDevice(NULL, type, NULL, flags, &featureLevel, 1, D3D11_SDK_VERSION, &pDevice, NULL, &pContext));
		CheckHR(pDevice.As(&g->mRenderDevice));
		CheckHR(pContext.As(&g->mRenderContext));
	}

	{
		D3D11_INPUT_ELEMENT_DESC desc[] =
		{
			{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
			{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
		};

		ComPtr<ID3D11InputLayout> pInputLayout;
		CheckHR(g->mRenderDevice->CreateInputLayout(desc, ARRAYSIZE(desc), kVertexShader, ARRAYSIZE(kVertexShader), &pInputLayout));
		g->mRenderContext->IASetInputLayout(pInputLayout.Get());
		g->mRenderContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	}

	{
		ComPtr<ID3D11VertexShader> pVertexShader;
		CheckHR(g->mRenderDevice->CreateVertexShader(kVertexShader, ARRAYSIZE(kVertexShader), NULL, &pVertexShader));
		g->mRenderContext->VSSetShader(pVertexShader.Get(), NULL, 0);
	}

	{
		ComPtr<ID3D11PixelShader> pPixelShader;
		CheckHR(g->mRenderDevice->CreatePixelShader(kPixelShader, ARRAYSIZE(kPixelShader), NULL, &pPixelShader));
		g->mRenderContext->PSSetShader(pPixelShader.Get(), NULL, 0);
	}

	{
		CD3D11_SAMPLER_DESC desc(D3D11_DEFAULT);
		ComPtr<ID3D11SamplerState> pSampler;
		CheckHR(g->mRenderDevice->CreateSamplerState(&desc, &pSampler));
		g->mRenderContext->PSSetSamplers(0, 1, pSampler.GetAddressOf());
	}

	{
		CD3D11_TEXTURE2D_DESC desc(kFormat, 256, 256); // 2x2 tiles for testing, we only use the first tile though
		desc.MipLevels = 1;
		desc.MiscFlags = D3D11_RESOURCE_MISC_TILED;

		CheckHR(g->mRenderDevice->CreateTexture2D(&desc, NULL, &g->mTexture));
	}

	{
		D3D11_SHADER_RESOURCE_VIEW_DESC desc;
		desc.Format = kFormat;
		desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		desc.Texture2D.MostDetailedMip = 0;
		desc.Texture2D.MipLevels = 1;

		ComPtr<ID3D11ShaderResourceView> pTextureView;
		CheckHR(g->mRenderDevice->CreateShaderResourceView(g->mTexture.Get(), &desc, &pTextureView));

		g->mRenderContext->PSSetShaderResources(0, 1, pTextureView.GetAddressOf());
	}

	{
		CD3D11_BUFFER_DESC desc(64 << 10, 0);
		desc.MiscFlags = D3D11_RESOURCE_MISC_TILE_POOL;

		CheckHR(g->mRenderDevice->CreateBuffer(&desc, NULL, &g->mTextureBuffer));

		D3D11_TILE_REGION_SIZE box = { 1, FALSE };
		UINT tileId = 0;
		CheckHR(g->mRenderContext->UpdateTileMappings(g->mTexture.Get(), 1, NULL, &box, g->mTextureBuffer.Get(), 1, NULL, &tileId, NULL, 0));
	}

	{
		// clear tile #0 to red (corruption is not visible because we write the whole tile and we use a uniform color)

		static int buffer[128 * 128];
		for (int i = 0; i < 128 * 128; ++i)
			buffer[i] = 0xFF800000;

		D3D11_TILED_RESOURCE_COORDINATE pos = { 0, 0 };
		D3D11_TILE_REGION_SIZE box = { 1, FALSE };
		g->mRenderContext->UpdateTiles(g->mTexture.Get(), &pos, &box, buffer, 0);
	}

	{
		struct Vertex
		{
			XMFLOAT3 pos;
			XMFLOAT2 tex;
		};

		float x = mWidth * 0.1f;
		float y = mHeight * 0.1f;
		float w = mWidth * 0.8f;
		float h = mHeight * 0.8f;

		float t0 = 1.0f / 256.0f;
		float t1 = 127.0f / 256.0f;

		Vertex vertices[] =
		{
			{ { x    , y    , 0 } , { t0, t0 } },
			{ { x + w, y    , 0 } , { t1, t0 } },
			{ { x    , y + h, 0 } , { t0, t1 } },
			{ { x    , y + h, 0 } , { t0, t1 } },
			{ { x + w, y    , 0 } , { t1, t0 } },
			{ { x + w, y + h, 0 } , { t1, t1 } },
		};

		CD3D11_BUFFER_DESC desc(sizeof(Vertex) * ARRAYSIZE(vertices), D3D11_BIND_VERTEX_BUFFER, D3D11_USAGE_IMMUTABLE);
		D3D11_SUBRESOURCE_DATA data = { vertices };
		CheckHR(g->mRenderDevice->CreateBuffer(&desc, &data, &g->mVertexBuffer));

		UINT stride = sizeof(Vertex);
		UINT offset = 0;
		g->mRenderContext->IASetVertexBuffers(0, 1, g->mVertexBuffer.GetAddressOf(), &stride, &offset);
	}

	{
		XMFLOAT4X4 matrix;
		XMStoreFloat4x4(&matrix, XMMatrixOrthographicOffCenterLH(0, mWidth, mHeight, 0, -1, +1));

		CD3D11_BUFFER_DESC desc(sizeof(XMFLOAT4X4), D3D11_BIND_CONSTANT_BUFFER, D3D11_USAGE_IMMUTABLE);
		D3D11_SUBRESOURCE_DATA data = { &matrix };

		ComPtr<ID3D11Buffer> pCameraBufer;
		CheckHR(g->mRenderDevice->CreateBuffer(&desc, &data, &pCameraBufer));

		g->mRenderContext->VSSetConstantBuffers(0, 1, pCameraBufer.GetAddressOf());
	}

	{
		DXGI_SWAP_CHAIN_DESC1 desc = {};
		desc.Format = kFormat;
		desc.SampleDesc.Count = 1;
		desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		desc.BufferCount = 2;
		desc.Scaling = DXGI_SCALING_STRETCH;
		desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
		desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

		ComPtr<IDXGISwapChain1> pSwapChain;
		CheckHR(g->mGraphicsFactory->CreateSwapChainForHwnd(g->mRenderDevice.Get(), hwnd, &desc, NULL, NULL, &pSwapChain));
		CheckHR(pSwapChain.As(&g->mSwapChain));
	}
}

void RenderFrame(HWND hwnd)
{
	auto rc = g->mRenderContext.Get();

	if (!g->mRenderTarget)
	{
		ComPtr<ID3D11Texture2D> pTexture;
		CheckHR(g->mSwapChain->GetBuffer(0, IID_PPV_ARGS(&pTexture)));

		D3D11_RENDER_TARGET_VIEW_DESC desc;
		desc.Format = kFormat;
		desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
		desc.Texture2D.MipSlice = 0;
	
		CheckHR(g->mRenderDevice->CreateRenderTargetView(pTexture.Get(), &desc, &g->mRenderTarget));
	}

	rc->OMSetRenderTargets(1, g->mRenderTarget.GetAddressOf(), NULL);

	D3D11_VIEWPORT viewport = { 0, 0, mWidth, mHeight, 0, 1 };
	rc->RSSetViewports(1, &viewport);

	FLOAT bg[] = { 0, 0, 0.5, 1 };
	rc->ClearRenderTargetView(g->mRenderTarget.Get(), bg);

	rc->Draw(6, 0);

	CheckHR(g->mSwapChain->Present(0, 0));
}

LRESULT CALLBACK MainWindowProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
	switch (msg)
	{
	case WM_SIZE:
	{
		mWidth = (float)LOWORD(lparam);
		mHeight = (float)HIWORD(lparam);

		if (g->mSwapChain)
		{
			g->mRenderContext->OMSetRenderTargets(0, NULL, NULL);
			g->mRenderTarget.Reset();
			CheckHR(g->mSwapChain->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN, 0));
		}

		break;
	}

	case WM_ERASEBKGND:
		return 1;

	case WM_PAINT:
	{
		RECT rc;
		auto hasInvalidRegion = GetUpdateRect(hwnd, &rc, FALSE);
		ValidateRect(hwnd, NULL);

		if (hasInvalidRegion && g->mSwapChain)
			RenderFrame(hwnd);

		return 0;
	}

	case WM_LBUTTONDOWN:
	{
		// load an image into tile #0

		ComPtr<IWICImagingFactory2> pFactory;
		CheckHR(CoCreateInstance(CLSID_WICImagingFactory2, NULL, CLSCTX_INPROC, IID_PPV_ARGS(&pFactory)));

		ComPtr<IWICBitmapDecoder> pDecoder;
		CheckHR(pFactory->CreateDecoderFromFilename(L"image.png", NULL, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &pDecoder));

		ComPtr<IWICBitmapFrameDecode> pFrame;
		CheckHR(pDecoder->GetFrame(0, &pFrame));

		ComPtr<IWICFormatConverter> pConverter;
		CheckHR(pFactory->CreateFormatConverter(&pConverter));
		CheckHR(pConverter->Initialize(pFrame.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, NULL, 0, WICBitmapPaletteTypeMedianCut));

		ComPtr<IWICBitmapScaler> pScaler;
		pFactory->CreateBitmapScaler(&pScaler);
		CheckHR(pScaler->Initialize(pConverter.Get(), 128, 128, WICBitmapInterpolationModeFant));

		static byte buffer[64 << 10];
		WICRect rc = { 0, 0, 128, 128 };
		CheckHR(pScaler->CopyPixels(&rc, 128 * 4, ARRAYSIZE(buffer), buffer));

		D3D11_TILED_RESOURCE_COORDINATE pos = { 0, 0 };
		D3D11_TILE_REGION_SIZE box = { 1, FALSE };
		g->mRenderContext->UpdateTiles(g->mTexture.Get(), &pos, &box, buffer, 0);
		InvalidateRect(hwnd, NULL, FALSE);
		break;
	}

	case WM_RBUTTONDOWN:
	{
		// clear tile #0 to green (corruption is not visible because we write the whole tile and we use a uniform color)

		static int buffer[128 * 128];
		for (int i = 0; i < 128 * 128; ++i)
			buffer[i] = 0xFF008000;

		D3D11_TILED_RESOURCE_COORDINATE pos = { 0, 0 };
		D3D11_TILE_REGION_SIZE box = { 1, FALSE };
		g->mRenderContext->UpdateTiles(g->mTexture.Get(), &pos, &box, buffer, 0);
	}
	{
		// load an image into a subregion of tile #0

		ComPtr<IWICImagingFactory2> pFactory;
		CheckHR(CoCreateInstance(CLSID_WICImagingFactory2, NULL, CLSCTX_INPROC, IID_PPV_ARGS(&pFactory)));

		ComPtr<IWICBitmapDecoder> pDecoder;
		CheckHR(pFactory->CreateDecoderFromFilename(L"image.png", NULL, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &pDecoder));

		ComPtr<IWICBitmapFrameDecode> pFrame;
		CheckHR(pDecoder->GetFrame(0, &pFrame));

		ComPtr<IWICFormatConverter> pConverter;
		CheckHR(pFactory->CreateFormatConverter(&pConverter));
		CheckHR(pConverter->Initialize(pFrame.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, NULL, 0, WICBitmapPaletteTypeMedianCut));

		ComPtr<IWICBitmapScaler> pScaler;
		pFactory->CreateBitmapScaler(&pScaler);
		CheckHR(pScaler->Initialize(pConverter.Get(), 32, 32, WICBitmapInterpolationModeFant));

		static byte buffer[32 * 32 * 4];
		WICRect rc = { 0, 0, 32, 32 };
		CheckHR(pScaler->CopyPixels(&rc, 32 * 4, ARRAYSIZE(buffer), buffer));

		D3D11_BOX box;
		box.left = 12;
		box.top = 12;
		box.front = 0;
		box.right = box.left + 32;
		box.bottom = box.top + 32;
		box.back = 1;
		g->mRenderContext->UpdateSubresource(g->mTexture.Get(), 0, &box, buffer, 32 * 4, 32 * 32 * 4);
	}
	{
		InvalidateRect(hwnd, NULL, FALSE);
		break;
	}

	case WM_CLOSE:
		PostQuitMessage(S_OK);
		break;
	}

	return DefWindowProcW(hwnd, msg, wparam, lparam);
}

HRESULT MessageLoop()
{
	for (;;)
	{
		MSG msg;
		switch (GetMessageW(&msg, NULL, 0, 0))
		{
		case -1:
			FailLastWin32();

		case 0:
			Check(msg.message == WM_QUIT);
			return (HRESULT)msg.wParam;

		default:
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
		}
	}
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPWSTR lpCmdLine, int nShowCmd)
{
	CheckHR(CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE));

	g = new Globals;

	WNDCLASSW wc = {};
	wc.hInstance = hInstance;
	wc.lpszClassName = kWindowClass;
	wc.lpfnWndProc = &MainWindowProc;
	wc.style = CS_HREDRAW | CS_VREDRAW;
	wc.hIcon = LoadIconW(NULL, IDI_APPLICATION);
	wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
	CheckLastWin32(!!RegisterClassW(&wc));

	HWND hwnd = CreateWindowExW(
		WS_EX_OVERLAPPEDWINDOW | WS_EX_NOREDIRECTIONBITMAP,
		kWindowClass,
		L"Tiled Texture Test",
		WS_OVERLAPPEDWINDOW,
		100, 100, 600, 600,
		NULL, NULL, hInstance, NULL);
	CheckLastWin32(hwnd != NULL);

	ShowWindow(hwnd, SW_SHOWDEFAULT);
	InitializeDevice(hwnd);
	UpdateWindow(hwnd);

	HRESULT result = MessageLoop();

	delete g;
	g = NULL;

	CoUninitialize();

	return result;
}
