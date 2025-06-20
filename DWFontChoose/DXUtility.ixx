module;
#include <d2d1.h>
#include <d2d1_1.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <dwrite_3.h>
#include <cassert>
#include <cmath>
#include <string>
#include <vector>
export module DXUtility;

#pragma comment(lib, "d2d1")
#pragma comment (lib, "d3d11") 
#pragma comment(lib, "dwrite")

export namespace DXUT {
	template<typename TCom> requires requires(TCom* pTCom) { pTCom->AddRef(); pTCom->Release(); }
	class comptr {
	public:
		comptr() = default;
		comptr(TCom* pTCom) : m_pTCom(pTCom) { }
		comptr(const comptr<TCom>& rhs) : m_pTCom(rhs.get()) { safe_addref(); }
		~comptr() { safe_release(); }
		operator TCom*()const { return get(); }
		operator TCom**() { return get_addr(); }
		operator IUnknown**() { return reinterpret_cast<IUnknown**>(get_addr()); }
		operator void**() { return reinterpret_cast<void**>(get_addr()); }
		auto operator->()const->TCom* { return get(); }
		auto operator=(const comptr<TCom>& rhs)->comptr& {
			if (this != &rhs) {
				safe_release();	m_pTCom = rhs.get(); safe_addref();
			}
			return *this;
		}
		auto operator=(TCom* pRHS)->comptr& {
			if (get() != pRHS) {
				if (get() != nullptr) { get()->Release(); }
				m_pTCom = pRHS;
			}
			return *this;
		}
		[[nodiscard]] bool operator==(const comptr<TCom>& rhs)const { return get() == rhs.get(); }
		[[nodiscard]] bool operator==(const TCom* pRHS)const { return get() == pRHS; }
		[[nodiscard]] explicit operator bool() { return get() != nullptr; }
		[[nodiscard]] explicit operator bool()const { return get() != nullptr; }
		[[nodiscard]] auto get()const -> TCom* { return m_pTCom; }
		[[nodiscard]] auto get_addr() -> TCom** { return &m_pTCom; }
		void safe_release() { if (get() != nullptr) { get()->Release(); m_pTCom = nullptr; } }
		void safe_addref() { if (get() != nullptr) { get()->AddRef(); } }
	private:
		TCom* m_pTCom { };
	};

	[[nodiscard]] inline auto D2DGetFactory() -> ID2D1Factory1* {
		static const comptr pD2DFactory1 = []() {
			ID2D1Factory1* pFactory1;
			::D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1),
				reinterpret_cast<void**>(&pFactory1));
			assert(pFactory1);
			return pFactory1;
			} ();
		return pD2DFactory1;
	}

	[[nodiscard]] inline auto D3D11GetDevice() -> ID3D11Device* {
		static const comptr pD3D11Device = []() {
			UINT uDeviceFlags = D3D11_CREATE_DEVICE_SINGLETHREADED | D3D11_CREATE_DEVICE_BGRA_SUPPORT;
		#ifdef _DEBUG
			uDeviceFlags |= D3D11_CREATE_DEVICE_DEBUG;
		#endif
			const D3D_FEATURE_LEVEL arrFL[] {
				D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0,
				D3D_FEATURE_LEVEL_9_3, D3D_FEATURE_LEVEL_9_2, D3D_FEATURE_LEVEL_9_1
			};
			ID3D11Device* pDevice;
			::D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, uDeviceFlags, arrFL, std::size(arrFL),
				D3D11_SDK_VERSION, &pDevice, nullptr, nullptr);
			assert(pDevice);
			return pDevice; }
		();
		return pD3D11Device;
	}

	[[nodiscard]] inline auto DXGIGetDevice() -> IDXGIDevice1* {
		static const comptr pDXGIDevice1 = []() {
			IDXGIDevice1* pDevice1;
			D3D11GetDevice()->QueryInterface(&pDevice1);
			assert(pDevice1);
			return pDevice1;
			}();
		return pDXGIDevice1;
	}

	[[nodiscard]] inline auto DXGICreateSwapChainForHWND(HWND hWnd) -> IDXGISwapChain1* {
		assert(::IsWindow(hWnd));
		const DXGI_SWAP_CHAIN_DESC1 scd { .Width { 0 }, .Height { 0 }, .Format { DXGI_FORMAT_B8G8R8A8_UNORM },
			.Stereo { false }, .SampleDesc { .Count { 1 }, .Quality { 0 } },
			.BufferUsage { DXGI_USAGE_RENDER_TARGET_OUTPUT }, .BufferCount { 2 }, .Scaling { DXGI_SCALING_NONE },
			.SwapEffect { DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL }, .AlphaMode { DXGI_ALPHA_MODE_UNSPECIFIED }, .Flags { 0 } };
		comptr<IDXGIAdapter> pDXGIAdapter;
		DXGIGetDevice()->GetAdapter(pDXGIAdapter);
		assert(pDXGIAdapter);
		if (!pDXGIAdapter) { return { }; }

		comptr<IDXGIFactory2> pDXGIFactory2;
		pDXGIAdapter->GetParent(__uuidof(**(pDXGIFactory2.get_addr())), pDXGIFactory2);
		assert(pDXGIFactory2);
		if (!pDXGIFactory2) { return { }; }

		IDXGISwapChain1* pDXGISwapChain1;
		pDXGIFactory2->CreateSwapChainForHwnd(D3D11GetDevice(), hWnd, &scd, nullptr, nullptr, &pDXGISwapChain1);
		assert(pDXGISwapChain1);
		return pDXGISwapChain1;
	}

	[[nodiscard]] inline auto D2DCreateDevice() -> ID2D1Device* {
		ID2D1Device* pD2DDevice;
		D2DGetFactory()->CreateDevice(DXGIGetDevice(), &pD2DDevice);
		assert(pD2DDevice);
		return pD2DDevice;
	}

	[[nodiscard]] inline auto D2DCreateDeviceContext(ID2D1Device* pD2DDevice) -> ID2D1DeviceContext* {
		assert(pD2DDevice);
		ID2D1DeviceContext* pD2DDeviceContext;
		pD2DDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &pD2DDeviceContext);
		assert(pD2DDeviceContext);
		return pD2DDeviceContext;
	}

	[[nodiscard]] inline auto D2DCreateBitmapFromDxgiSurface(ID2D1DeviceContext* pD2DDC, IDXGISwapChain1* pDXGISwapChain) -> ID2D1Bitmap1* {
		assert(pD2DDC);
		assert(pDXGISwapChain);
		comptr<IDXGISurface> pDXGISurface;
		pDXGISwapChain->GetBuffer(0, __uuidof(**(pDXGISurface.get_addr())), pDXGISurface);
		assert(pDXGISurface);
		if (!pDXGISurface) { return{ }; }

		ID2D1Bitmap1* pD2DBitmap1;
		const auto bp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
		   D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
		pD2DDC->CreateBitmapFromDxgiSurface(pDXGISurface, bp, &pD2DBitmap1);
		return pD2DBitmap1;
	}

	[[nodiscard]] inline auto DWGetFactory() -> IDWriteFactory3* {
		static const comptr pD2DWriteFactory = []() {
			IDWriteFactory3* pWriteFactory;
			::DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory3),
				reinterpret_cast<IUnknown**>(&pWriteFactory));
			assert(pWriteFactory);
			return pWriteFactory;
			} ();
		return pD2DWriteFactory;
	}

	struct DWFONTINFO {
		std::wstring        wstrFamilyName;
		std::wstring        wstrLocale;
		DWRITE_FONT_WEIGHT  eWeight { DWRITE_FONT_WEIGHT_NORMAL };
		DWRITE_FONT_STRETCH eStretch { DWRITE_FONT_STRETCH_NORMAL };
		DWRITE_FONT_STYLE   eStyle { DWRITE_FONT_STYLE_NORMAL };
		float               flSizeDIP { }; //Font size in Device Independent Pixels (not points).
	};

	[[nodiscard]] inline auto DWCreateTextFormat(const DWFONTINFO& fi) -> IDWriteTextFormat1* {
		comptr<IDWriteTextFormat> pTextFormat;
		DWGetFactory()->CreateTextFormat(fi.wstrFamilyName.data(), nullptr, fi.eWeight, fi.eStyle, fi.eStretch,
			fi.flSizeDIP, fi.wstrLocale.data(), pTextFormat);
		assert(pTextFormat);
		if (!pTextFormat) { return { }; }

		IDWriteTextFormat1* pTextFormat1;
		pTextFormat->QueryInterface(&pTextFormat1);
		return pTextFormat1;
	}

	[[nodiscard]] inline auto DWCreateTextLayout(std::wstring_view wsv, IDWriteTextFormat1* pTextFormat, float flWidthMax,
		float flHeightMax) -> IDWriteTextLayout1* {
		assert(pTextFormat);
		comptr<IDWriteTextLayout> pTextLayout;
		DWGetFactory()->CreateTextLayout(wsv.data(), static_cast<UINT32>(wsv.size()), pTextFormat, flWidthMax,
			flHeightMax, pTextLayout);
		assert(pTextLayout);
		if (!pTextLayout) { return { }; }

		IDWriteTextLayout1* pTextLayout1;
		pTextLayout->QueryInterface(&pTextLayout1);
		return pTextLayout1;
	}

	[[nodiscard]] inline auto DWCreateGDITextLayout(std::wstring_view wsv, IDWriteTextFormat1* pTextFormat, float flWidthMax,
		float flHeightMax) -> IDWriteTextLayout1* {
		assert(pTextFormat);
		comptr<IDWriteTextLayout> pTextLayout;
		DWGetFactory()->CreateGdiCompatibleTextLayout(wsv.data(), static_cast<UINT32>(wsv.size()), pTextFormat, flWidthMax,
			flHeightMax, 1.F, nullptr, FALSE, pTextLayout);
		assert(pTextLayout);
		if (!pTextLayout) { return { }; }

		IDWriteTextLayout1* pTextLayout1;
		pTextLayout->QueryInterface(&pTextLayout1);
		return pTextLayout1;
	}

	struct DWFONTFACEINFO {
		std::wstring wstrTypographicFamilyName;           //DWRITE_FONT_PROPERTY_ID_TYPOGRAPHIC_FAMILY_NAME
		std::wstring wstrWeightStretchStyleFaceName;      //DWRITE_FONT_PROPERTY_ID_WEIGHT_STRETCH_STYLE_FACE_NAME
		std::wstring wstrFullName;                        //DWRITE_FONT_PROPERTY_ID_FULL_NAME
		std::wstring wstrWin32FamilyName;                 //DWRITE_FONT_PROPERTY_ID_WIN32_FAMILY_NAME
		std::wstring wstrPostScriptName;                  //DWRITE_FONT_PROPERTY_ID_POSTSCRIPT_NAME
		std::vector<std::wstring> vecDesignScriptLangTag; //DWRITE_FONT_PROPERTY_ID_DESIGN_SCRIPT_LANGUAGE_TAG
		std::vector<std::wstring> vecSuppScriptLangTag;   //DWRITE_FONT_PROPERTY_ID_SUPPORTED_SCRIPT_LANGUAGE_TAG
		std::vector<std::wstring> vecSemanticTag;         //DWRITE_FONT_PROPERTY_ID_SEMANTIC_TAG
		std::wstring wstrWeight;                          //DWRITE_FONT_PROPERTY_ID_WEIGHT
		std::wstring wstrStretch;                         //DWRITE_FONT_PROPERTY_ID_STRETCH
		std::wstring wstrStyle;                           //DWRITE_FONT_PROPERTY_ID_STYLE
		std::wstring wstrTypographicFaceName;             //DWRITE_FONT_PROPERTY_ID_TYPOGRAPHIC_FACE_NAME
	};

	struct DWFONTFAMILYINFO {
		std::wstring                wstrFamilyName; //DWRITE_FONT_PROPERTY_ID_WEIGHT_STRETCH_STYLE_FAMILY_NAME
		std::wstring                wstrLocale;
		std::vector<DWFONTFACEINFO> vecFontFaceInfo;
	};

	[[nodiscard]] inline auto DWGetSystemFonts(const wchar_t* pwszLocale = L"en-US") -> std::vector<DWFONTFAMILYINFO> {
		const auto lmbGetWstrLocale = [=](IDWriteLocalizedStrings* pLocStrings)->std::wstring {
			if (pLocStrings == nullptr) {
				return { };
			}

			UINT32 u32Index;
			BOOL fExist;
			if (pLocStrings->FindLocaleName(pwszLocale, &u32Index, &fExist); fExist) {
				wchar_t buff[64];
				pLocStrings->GetString(u32Index, buff, std::size(buff));
				return buff;
			}
			return { }; };
		const auto lmbGetWstrFirst = [](IDWriteLocalizedStrings* pLocStrings)->std::wstring {
			if (pLocStrings == nullptr) {
				return { };
			}

			wchar_t buff[64];
			pLocStrings->GetString(0, buff, std::size(buff));
			return buff;
			};
		const auto lmbGetWstrAll = [](IDWriteLocalizedStrings* pLocStrings)->std::vector<std::wstring> {
			if (pLocStrings == nullptr) {
				return { };
			}

			const auto sCount = pLocStrings->GetCount();
			std::vector<std::wstring> vec;
			vec.reserve(sCount);
			for (auto i = 0U; i < sCount; ++i) {
				wchar_t buff[64];
				pLocStrings->GetString(i, buff, std::size(buff));
				vec.emplace_back(buff);
			}
			return vec;
			};

		comptr<IDWriteFontSet> pSysFontSet;
		DWGetFactory()->GetSystemFontSet(pSysFontSet);
		assert(pSysFontSet);
		if (!pSysFontSet) { return{ }; }

		comptr<IDWriteStringList> pStringsFamilyName;
		pSysFontSet->GetPropertyValues(DWRITE_FONT_PROPERTY_ID_WEIGHT_STRETCH_STYLE_FAMILY_NAME, pwszLocale,
			pStringsFamilyName);
		assert(pStringsFamilyName);
		if (!pStringsFamilyName) { return{ }; }

		const auto iCountFontFamilies = pStringsFamilyName->GetCount(); //How many unique Font Family Names.
		std::vector<DWFONTFAMILYINFO> vecFontInfo;
		vecFontInfo.reserve(iCountFontFamilies);

		for (auto iFontFamily = 0U; iFontFamily < iCountFontFamilies; ++iFontFamily) {
			wchar_t buffFamilyName[64];
			pStringsFamilyName->GetString(iFontFamily, buffFamilyName, std::size(buffFamilyName));
			const DWRITE_FONT_PROPERTY fp { .propertyId { DWRITE_FONT_PROPERTY_ID_WEIGHT_STRETCH_STYLE_FAMILY_NAME },
				.propertyValue { buffFamilyName } };
			comptr<IDWriteFontSet> pFamilyNameSet;
			pSysFontSet->GetMatchingFonts(&fp, 1, pFamilyNameSet);
			const auto iCountFontFaces = pFamilyNameSet->GetFontCount(); //How many fonts (Font Face) within this Family Name.
			std::vector<DWFONTFACEINFO> vecFontFaceInfo;
			vecFontFaceInfo.reserve(iCountFontFaces);

			for (auto iFontFace = 0U; iFontFace < iCountFontFaces; ++iFontFace) {
				BOOL f;
				comptr<IDWriteLocalizedStrings> pStrTypographicFamilyName;
				comptr<IDWriteLocalizedStrings> pStrWeightStretchStyleFaceName;
				comptr<IDWriteLocalizedStrings> pStrFullName;
				comptr<IDWriteLocalizedStrings> pStrWin32FamilyName;
				comptr<IDWriteLocalizedStrings> pStrPostScriptName;
				comptr<IDWriteLocalizedStrings> pStrDesignScriptLangTag;
				comptr<IDWriteLocalizedStrings> pStrSuppScriptLangTag;
				comptr<IDWriteLocalizedStrings> pStrSemanticTag;
				comptr<IDWriteLocalizedStrings> pStrWeight;
				comptr<IDWriteLocalizedStrings> pStrStretch;
				comptr<IDWriteLocalizedStrings> pStrStyle;
				comptr<IDWriteLocalizedStrings> pStrTypographicFaceName;
				pFamilyNameSet->GetPropertyValues(iFontFace, DWRITE_FONT_PROPERTY_ID_TYPOGRAPHIC_FAMILY_NAME, &f, pStrTypographicFamilyName);
				pFamilyNameSet->GetPropertyValues(iFontFace, DWRITE_FONT_PROPERTY_ID_WEIGHT_STRETCH_STYLE_FACE_NAME, &f, pStrWeightStretchStyleFaceName);
				pFamilyNameSet->GetPropertyValues(iFontFace, DWRITE_FONT_PROPERTY_ID_FULL_NAME, &f, pStrFullName);
				pFamilyNameSet->GetPropertyValues(iFontFace, DWRITE_FONT_PROPERTY_ID_WIN32_FAMILY_NAME, &f, pStrWin32FamilyName);
				pFamilyNameSet->GetPropertyValues(iFontFace, DWRITE_FONT_PROPERTY_ID_POSTSCRIPT_NAME, &f, pStrPostScriptName);
				pFamilyNameSet->GetPropertyValues(iFontFace, DWRITE_FONT_PROPERTY_ID_DESIGN_SCRIPT_LANGUAGE_TAG, &f, pStrDesignScriptLangTag);
				pFamilyNameSet->GetPropertyValues(iFontFace, DWRITE_FONT_PROPERTY_ID_SUPPORTED_SCRIPT_LANGUAGE_TAG, &f, pStrSuppScriptLangTag);
				pFamilyNameSet->GetPropertyValues(iFontFace, DWRITE_FONT_PROPERTY_ID_SEMANTIC_TAG, &f, pStrSemanticTag);
				pFamilyNameSet->GetPropertyValues(iFontFace, DWRITE_FONT_PROPERTY_ID_WEIGHT, &f, pStrWeight);
				pFamilyNameSet->GetPropertyValues(iFontFace, DWRITE_FONT_PROPERTY_ID_STRETCH, &f, pStrStretch);
				pFamilyNameSet->GetPropertyValues(iFontFace, DWRITE_FONT_PROPERTY_ID_STYLE, &f, pStrStyle);
				pFamilyNameSet->GetPropertyValues(iFontFace, DWRITE_FONT_PROPERTY_ID_TYPOGRAPHIC_FACE_NAME, &f, pStrTypographicFaceName);
				vecFontFaceInfo.emplace_back(DWFONTFACEINFO {
					.wstrTypographicFamilyName { lmbGetWstrLocale(pStrTypographicFamilyName) },
					.wstrWeightStretchStyleFaceName { lmbGetWstrLocale(pStrWeightStretchStyleFaceName) },
					.wstrFullName { lmbGetWstrLocale(pStrFullName) },
					.wstrWin32FamilyName { lmbGetWstrLocale(pStrWin32FamilyName) },
					.wstrPostScriptName { lmbGetWstrLocale(pStrPostScriptName) },
					.vecDesignScriptLangTag { lmbGetWstrAll(pStrDesignScriptLangTag) },
					.vecSuppScriptLangTag { lmbGetWstrAll(pStrSuppScriptLangTag) },
					.vecSemanticTag { lmbGetWstrAll(pStrSemanticTag) },
					.wstrWeight { lmbGetWstrFirst(pStrWeight) },
					.wstrStretch { lmbGetWstrFirst(pStrStretch) },
					.wstrStyle { lmbGetWstrFirst(pStrStyle) },
					.wstrTypographicFaceName { lmbGetWstrLocale(pStrTypographicFaceName) } });
			}

			vecFontInfo.emplace_back(DWFONTFAMILYINFO { .wstrFamilyName { buffFamilyName }, .wstrLocale { pwszLocale },
				.vecFontFaceInfo { std::move(vecFontFaceInfo) } });
		}

		return vecFontInfo;
	}

	struct DRAWCONTEXT {
		ID2D1DeviceContext* pDeviceContext { };
		IDWriteTextLayout1* pTextLayout { };
		ID2D1Brush*         pBrushTextDef { }; //Default text brush.
	};

	class CTextEffect final : public IUnknown {
	public:
		CTextEffect() = default;
		CTextEffect(ID2D1Brush* pBrushBk, ID2D1Brush* pBrushText) : m_pBrushBk(pBrushBk), m_pBrushText(pBrushText) { }
		auto AddRef() -> ULONG override { return 1UL; }
		auto Release() -> ULONG override { return 1UL; }
		auto QueryInterface([[maybe_unused]] const IID& riid, [[maybe_unused]] void** ppvObject) -> HRESULT override { return E_NOTIMPL; }
		[[nodiscard]] auto GetBkBrush()const -> ID2D1Brush* { return m_pBrushBk; };
		[[nodiscard]] auto GetTextBrush()const -> ID2D1Brush* { return m_pBrushText; };
		void SetBkBrush(ID2D1Brush* pBrushBk) { m_pBrushBk = pBrushBk; }
		void SetTextBrush(ID2D1Brush* pBrushText) { m_pBrushText = pBrushText; }
	private:
		ID2D1Brush* m_pBrushBk { };
		ID2D1Brush* m_pBrushText { };
	};

	class CDWriteTextRenderer final : public IDWriteTextRenderer {
	public:
		void SetDrawContext(const DRAWCONTEXT& context) { m_context = context; }
	public: //IDWriteTextRenderer overridden methods.
		auto AddRef() -> ULONG override { return 1UL; }
		auto Release() -> ULONG override { return 1UL; }
		auto QueryInterface(const IID& riid, void** ppvObject) -> HRESULT override {
			if (riid == __uuidof(IUnknown)) {
				*ppvObject = reinterpret_cast<IUnknown*>(this);
				return S_OK;
			}
			if (riid == __uuidof(IDWritePixelSnapping)) {
				*ppvObject = reinterpret_cast<IDWritePixelSnapping*>(this);
				return S_OK;
			}
			if (riid == __uuidof(IDWriteTextRenderer)) {
				*ppvObject = reinterpret_cast<IDWriteTextRenderer*>(this);
				return S_OK;
			}

			*ppvObject = nullptr;

			return E_NOINTERFACE;
		}
		auto DrawGlyphRun([[maybe_unused]] void* pContext, FLOAT flBaseLineX, FLOAT flBaseLineY, DWRITE_MEASURING_MODE eMMode,
			const DWRITE_GLYPH_RUN* pGR, [[maybe_unused]] const DWRITE_GLYPH_RUN_DESCRIPTION* pGRD, IUnknown* pEffect) -> HRESULT override {
			const auto pTextEffect = static_cast<CTextEffect*>(pEffect);
			ID2D1Brush* pBrushText;

			if (pTextEffect != nullptr) {
				const auto pBrushBk = pTextEffect->GetBkBrush();
				pBrushText = pTextEffect->GetTextBrush();

				float flTextWidth = 0;
				for (UINT32 i = 0; i < pGR->glyphCount; ++i) {
					flTextWidth += pGR->glyphAdvances[i];
				}

				DWRITE_FONT_METRICS fm;
				pGR->fontFace->GetMetrics(&fm);
				const auto flAdjust = pGR->fontEmSize / fm.designUnitsPerEm;
				const auto flAscent = fm.ascent * flAdjust;
				const auto flDescent = fm.descent * flAdjust;
				const auto rcBk = D2D1::RectF(flBaseLineX, flBaseLineY - flAscent,
					flBaseLineX + flTextWidth, flBaseLineY + flDescent);
				m_context.pDeviceContext->FillRectangle(rcBk, pBrushBk);
			}
			else { pBrushText = m_context.pBrushTextDef; }

			m_context.pDeviceContext->DrawGlyphRun(D2D1::Point2F(flBaseLineX, flBaseLineY), pGR, pBrushText, eMMode);

			return S_OK;
		}
		auto DrawInlineObject([[maybe_unused]] void* pContext, [[maybe_unused]] FLOAT flOriginX, [[maybe_unused]] FLOAT flOriginY,
			[[maybe_unused]] IDWriteInlineObject* pInlineObject, [[maybe_unused]] BOOL fIsSideways, [[maybe_unused]] BOOL fIsRightToLeft,
			[[maybe_unused]] IUnknown* pEffect) -> HRESULT override {
			return E_NOTIMPL;
		}
		auto DrawStrikethrough([[maybe_unused]] void* pContext, [[maybe_unused]] FLOAT flOriginX, [[maybe_unused]] FLOAT flOriginY,
			[[maybe_unused]] const DWRITE_STRIKETHROUGH* pStrikeThrough, [[maybe_unused]] IUnknown* pEffect) -> HRESULT override {
			return E_NOTIMPL;
		}
		auto DrawUnderline([[maybe_unused]] void* pContext, [[maybe_unused]] FLOAT flOriginX, [[maybe_unused]] FLOAT flOriginY,
			[[maybe_unused]] const DWRITE_UNDERLINE* pUnderline, [[maybe_unused]] IUnknown* pEffect) -> HRESULT override {
			return E_NOTIMPL;
		}
		auto GetCurrentTransform([[maybe_unused]] void* pContext, [[maybe_unused]] DWRITE_MATRIX* pMatrix) -> HRESULT override {
			m_context.pDeviceContext->GetTransform(reinterpret_cast<D2D1_MATRIX_3X2_F*>(pMatrix));
			return S_OK;
		}
		auto GetPixelsPerDip([[maybe_unused]] void* pContext, [[maybe_unused]] FLOAT* pPixelsPerDip) -> HRESULT override {
			float flDPIX;
			float flDPIY;
			m_context.pDeviceContext->GetDpi(&flDPIX, &flDPIY);
			*pPixelsPerDip = flDPIX / USER_DEFAULT_SCREEN_DPI;
			return S_OK;
		}
		auto IsPixelSnappingDisabled([[maybe_unused]] void* pContext, BOOL* pfIsDisabled) -> HRESULT override {
			*pfIsDisabled = FALSE;
			return S_OK;
		}
	private:
		DRAWCONTEXT m_context;
	};
}