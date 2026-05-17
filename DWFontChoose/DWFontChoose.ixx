/********************************************************************
* Copyright © 2025-present Jovibor https://github.com/jovibor/      *
* DirectWrite Font Chooser                                          *
* Official git repository: https://github.com/jovibor/DWFontChoose/ *
* This software is available under "The MIT License"                *
********************************************************************/
module;
#include <SDKDDKVer.h>
#include "DWFontChooseRes.h"
#include <Windows.h>
#include <commctrl.h>
#include <d2d1_1.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <dwrite_3.h>
#include <algorithm>
#include <cassert>
#include <cwctype>
#include <format>
#include <optional>
#include <ranges>
#include <source_location>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>
export module DWFontChoose;

#pragma comment(linker,"/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' \
version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#pragma comment(lib, "Comctl32")
#pragma comment(lib, "d2d1")
#pragma comment (lib, "d3d11") 
#pragma comment(lib, "dwrite")

namespace DWFONTCHOOSE {
	export enum class EDWFontFamily : std::uint8_t {
		FAMILY_ALL = 0x1, FAMILY_MONOSPACED, FAMILY_NONMONOSPACED
	};

	export struct DWFONTCHOOSEINFO {
		std::wstring wstrLocale { L"en-US" }; //Semicolon delimited language names in preferred order.
		HINSTANCE hInstRes { };               //HINSTANCE where all dialog resources reside.
		HWND hWndParent { };                  //Parent window.
		EDWFontFamily eFontFamily { EDWFontFamily::FAMILY_ALL }; //Which font families to display.
	};

	export struct DWFONTINFO {
		std::wstring        wstrFamilyName;
		std::wstring        wstrLocale;
		DWRITE_FONT_WEIGHT  eWeight { DWRITE_FONT_WEIGHT_NORMAL };
		DWRITE_FONT_STRETCH eStretch { DWRITE_FONT_STRETCH_NORMAL };
		DWRITE_FONT_STYLE   eStyle { DWRITE_FONT_STYLE_NORMAL };
		float               flSizeDIP { }; //Font size in Device Independent Pixels (not points).
	};

	namespace ut {
		[[nodiscard]] constexpr auto FontPointsFromPixels(float flSizePixels) -> float
		{
			constexpr auto flPointsInPixel = 72.F / USER_DEFAULT_SCREEN_DPI;
			return flSizePixels * flPointsInPixel;
		}

		[[nodiscard]] constexpr auto FontPixelsFromPoints(float flSizePoints) -> float
		{
			constexpr auto flPixelsInPoint = USER_DEFAULT_SCREEN_DPI / 72.F;
			return flSizePoints * flPixelsInPoint;
		}

		//Replicates GET_X_LPARAM macro from windowsx.h.
		[[nodiscard]] constexpr int GetXLPARAM(LPARAM lParam)
		{
			return (static_cast<int>(static_cast<short>(static_cast<WORD>((static_cast<DWORD_PTR>(lParam)) & 0xFFFFU))));
		}

		//Replicates GET_Y_LPARAM macro from windowsx.h.
		[[nodiscard]] constexpr int GetYLPARAM(LPARAM lParam)
		{
			return GetXLPARAM(lParam >> 16);
		}

		[[nodiscard]] auto StrToWstr(std::string_view sv, UINT uCodePage = CP_UTF8) -> std::wstring
		{
			const auto iSize = ::MultiByteToWideChar(uCodePage, 0, sv.data(), static_cast<int>(sv.size()), nullptr, 0);
			std::wstring wstr(iSize, 0);
			::MultiByteToWideChar(uCodePage, 0, sv.data(), static_cast<int>(sv.size()), wstr.data(), iSize);
			return wstr;
		}

		[[nodiscard]] auto WstrToStr(std::wstring_view wsv, UINT uCodePage = CP_UTF8) -> std::string
		{
			const auto iSize = ::WideCharToMultiByte(uCodePage, 0, wsv.data(), static_cast<int>(wsv.size()), nullptr, 0, nullptr, nullptr);
			std::string str(iSize, 0);
			::WideCharToMultiByte(uCodePage, 0, wsv.data(), static_cast<int>(wsv.size()), str.data(), iSize, nullptr, nullptr);
			return str;
		}

	#if defined(DEBUG) || defined(_DEBUG)
		void DBG_REPORT(const wchar_t* pMsg, const std::source_location& loc = std::source_location::current()) {
			::_wassert(pMsg, StrToWstr(loc.file_name()).data(), loc.line());
		}
	#else
		void DBG_REPORT([[maybe_unused]] const wchar_t*) { }
	#endif
	};

	namespace GDIUT { //Windows GDI related stuff.
		auto DefWndProc(const MSG& msg) -> LRESULT {
			return ::DefWindowProcW(msg.hwnd, msg.message, msg.wParam, msg.lParam);
		}

		template<typename T>
		auto CALLBACK WndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)->LRESULT {
			//Different IHexCtrl objects will share the same WndProc<ExactTypeHere> function.
			//Hence, the map is needed to differentiate these objects. 
			//The DlgProc<T> works absolutely the same way.
			static std::unordered_map<HWND, T*> uMap;

			//CREATESTRUCTW::lpCreateParams always possesses `this` pointer, passed to the CreateWindowExW
			//function as lpParam. We save it to the static uMap to have access to this->ProcessMsg() method.
			if (uMsg == WM_CREATE) {
				const auto lpCS = reinterpret_cast<LPCREATESTRUCTW>(lParam);
				if (lpCS->lpCreateParams != nullptr) {
					uMap[hWnd] = reinterpret_cast<T*>(lpCS->lpCreateParams);
				}
			}

			if (const auto it = uMap.find(hWnd); it != uMap.end()) {
				const auto ret = it->second->ProcessMsg({ .hwnd { hWnd }, .message { uMsg },
					.wParam { wParam }, .lParam { lParam } });
				if (uMsg == WM_NCDESTROY) { //Remove hWnd from the map on window destruction.
					uMap.erase(it);
				}
				return ret;
			}

			return ::DefWindowProcW(hWnd, uMsg, wParam, lParam);
		}

		template<typename T>
		auto CALLBACK DlgProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)->INT_PTR {
			//DlgProc should return zero for all non-processed messages.
			//In that case messages will be processed by Windows default dialog proc.
			//Non-processed messages should not be passed neither to DefWindowProcW nor to DefDlgProcW.
			//Processed messages should return any non-zero value, depending on message type.

			static std::unordered_map<HWND, T*> uMap;

			//DialogBoxParamW and CreateDialogParamW dwInitParam arg is sent with WM_INITDIALOG as lParam.
			if (uMsg == WM_INITDIALOG) {
				if (lParam != 0) {
					uMap[hWnd] = reinterpret_cast<T*>(lParam);
				}
			}

			if (const auto it = uMap.find(hWnd); it != uMap.end()) {
				const auto ret = it->second->ProcessMsg({ .hwnd { hWnd }, .message { uMsg },
					.wParam { wParam }, .lParam { lParam } });
				if (uMsg == WM_NCDESTROY) { //Remove hWnd from the map on dialog destruction.
					uMap.erase(it);
				}
				return ret;
			}

			return 0;
		}

		class CPoint final : public POINT {
		public:
			CPoint() : POINT { } { }
			CPoint(POINT pt) : POINT { pt } { }
			CPoint(int x, int y) : POINT { .x { x }, .y { y } } { }
			~CPoint() = default;
			operator LPPOINT() { return this; }
			operator const POINT*()const { return this; }
			[[nodiscard]] bool operator==(CPoint rhs)const { return x == rhs.x && y == rhs.y; }
			[[nodiscard]] bool operator==(POINT rhs)const { return x == rhs.x && y == rhs.y; }
			[[nodiscard]] friend bool operator==(POINT lhs, CPoint rhs) { return rhs == lhs; }
			CPoint operator+(POINT pt)const { return { x + pt.x, y + pt.y }; }
			CPoint operator-(POINT pt)const { return { x - pt.x, y - pt.y }; }
			void Offset(int iX, int iY) { x += iX; y += iY; }
			void Offset(POINT pt) { Offset(pt.x, pt.y); }
		};

		class CRect final : public RECT {
		public:
			CRect() : RECT { } { }
			CRect(int iLeft, int iTop, int iRight, int iBottom) : RECT { .left { iLeft }, .top { iTop },
				.right { iRight }, .bottom { iBottom } } { }
			CRect(const RECT& rc) { ::CopyRect(this, &rc); }
			CRect(LPCRECT pRC) { ::CopyRect(this, pRC); }
			CRect(POINT pt, SIZE size) : RECT { .left { pt.x }, .top { pt.y }, .right { pt.x + size.cx },
				.bottom { pt.y + size.cy } } { }
			CRect(POINT topLeft, POINT botRight) : RECT { .left { topLeft.x }, .top { topLeft.y },
				.right { botRight.x }, .bottom { botRight.y } } { }
			~CRect() = default;
			operator LPRECT() { return this; }
			operator LPCRECT()const { return this; }
			[[nodiscard]] bool operator==(const CRect& rhs)const { return ::EqualRect(this, rhs); }
			[[nodiscard]] bool operator==(const RECT& rhs)const { return ::EqualRect(this, &rhs); }
			[[nodiscard]] friend bool operator==(const RECT& lhs, const CRect& rhs) { return rhs == lhs; }
			CRect& operator=(const RECT& rhs) { ::CopyRect(this, &rhs); return *this; }
			CRect& operator=(const CRect& rhs) { ::CopyRect(this, &rhs); return *this; }
			[[nodiscard]] auto BottomRight()const -> CPoint { return { { .x { right }, .y { bottom } } }; };
			void DeflateRect(int x, int y) { ::InflateRect(this, -x, -y); }
			void DeflateRect(SIZE size) { ::InflateRect(this, -size.cx, -size.cy); }
			void DeflateRect(LPCRECT pRC) { left += pRC->left; top += pRC->top; right -= pRC->right; bottom -= pRC->bottom; }
			void DeflateRect(int l, int t, int r, int b) { left += l; top += t; right -= r; bottom -= b; }
			[[nodiscard]] int Height()const { return bottom - top; }
			[[nodiscard]] bool IsRectEmpty()const { return ::IsRectEmpty(this); }
			[[nodiscard]] bool IsRectNull()const { return (left == 0 && right == 0 && top == 0 && bottom == 0); }
			void OffsetRect(int x, int y) { ::OffsetRect(this, x, y); }
			void OffsetRect(POINT pt) { ::OffsetRect(this, pt.x, pt.y); }
			[[nodiscard]] bool PtInRect(POINT pt)const { return ::PtInRect(this, pt); }
			void SetRect(int x1, int y1, int x2, int y2) { ::SetRect(this, x1, y1, x2, y2); }
			void SetRectEmpty() { ::SetRectEmpty(this); }
			[[nodiscard]] auto TopLeft()const -> CPoint { return { { .x { left }, .y { top } } }; };
			[[nodiscard]] int Width()const { return right - left; }
		};

		class CSplitter final {
		public:
			enum class EAnchorSide : std::uint8_t { SIDE_LEFT, SIDE_TOP, SIDE_RIGHT, SIDE_BOTTOM };
			void AddItem(HWND hWndItem, bool fIsResize);
			void AddItem(int iItemID, bool fIsResize);
			void Initialize(HWND hWndHost, HWND hWndAnchor, EAnchorSide eAnchorSide, std::uint32_t u32SplitterWidth = 30);
			void Initialize(HWND hWndHost, int iAnchorID, EAnchorSide eAnchorSide, std::uint32_t u32SplitterWidth = 30);
			[[nodiscard]] bool IsSplitting()const; //Is splitting is going on atm.
			void SetEdges(int iMinEdge, int iMaxEdge);

			//These WM* handlers must be placed into the respective host window handlers.
			void WMMouseMove(int iX, int iY);
			void WMLButtonDown(int iX, int iY);
			void WMLButtonUp();
		private:
			[[nodiscard]] bool IsLock()const;
			void Lock();
			void Unlock();
		private:
			inline static CSplitter* m_pSplitterCurrentlyInUse { }; //The currently active splitter.
			struct ItemData {
				HWND hWnd { };      //Item window.
				bool fIsResize { }; //Is resize or move.
			};
			std::vector<ItemData> m_vecItems; //All items to resize/move.
			HWND m_hWndHost { };   //Host window.
			HWND m_hWndAnchor { }; //Anchor window, to work as a splitter basepoint.
			std::uint32_t m_u32WidthHalf { }; //Distance from the anchor-window's edge, where a cursor turns into a splitter (<->).
			POINT m_ptCurr;     //Current cursor coordinates under the splitter area.
			int m_iMinEdge { }; //Minimum distance from the left (or top) side to stop splitting.
			int m_iMaxEdge { 0x7FFFFFFF }; //Maximum distance from the left (or top) side to stop splitting.
			EAnchorSide m_eAnchorSide;
			bool m_fCurInSplitter { }; //Is cursor under the splitter area.
			bool m_fSplitting { };     //Left mouse is down for resize atm.
		};

		void CSplitter::AddItem(HWND hWndItem, bool fIsResize) {
			assert(hWndItem != nullptr);
			if (hWndItem == nullptr)
				return;

			m_vecItems.emplace_back(hWndItem, fIsResize);
		}

		void CSplitter::AddItem(int iItemID, bool fIsResize) {
			AddItem(::GetDlgItem(m_hWndHost, iItemID), fIsResize);
		}

		void CSplitter::Initialize(HWND hWndHost, HWND hWndAnchor, EAnchorSide eAnchorSide, std::uint32_t u32SplitterWidth) {
			assert(hWndHost != nullptr);
			assert(hWndAnchor != nullptr);
			m_hWndHost = hWndHost;
			m_hWndAnchor = hWndAnchor;
			m_eAnchorSide = eAnchorSide;
			m_u32WidthHalf = u32SplitterWidth / 2;
		}

		void CSplitter::Initialize(HWND hWndHost, int iAnchorID, EAnchorSide eAnchorSide, std::uint32_t u32SplitterWidth) {
			Initialize(hWndHost, ::GetDlgItem(hWndHost, iAnchorID), eAnchorSide, u32SplitterWidth);
		}

		bool CSplitter::IsSplitting()const {
			return m_fSplitting;
		}

		void CSplitter::SetEdges(int iMinEdge, int iMaxEdge) {
			m_iMinEdge = iMinEdge;
			m_iMaxEdge = iMaxEdge;
		}

		void CSplitter::WMLButtonDown(int iX, int iY) {
			if (IsLock()) {
				return;
			}

			if (m_fCurInSplitter) {
				m_ptCurr = POINT(iX, iY);
				m_fSplitting = true;
				Lock();
				::SetCapture(m_hWndHost);
			}
		}

		void CSplitter::WMLButtonUp() {
			if (IsLock()) {
				return;
			}

			if (m_fSplitting) {
				m_fSplitting = false;
				::ReleaseCapture();
				Unlock();
			}
		}

		void CSplitter::WMMouseMove(int iX, int iY) {
			if (IsLock()) {
				return;
			}

			const POINT pt { .x { iX }, .y { iY } };
			CRect rcAnchorClient;
			::GetWindowRect(m_hWndAnchor, &rcAnchorClient);
			::ScreenToClient(m_hWndHost, reinterpret_cast<LPPOINT>(&rcAnchorClient));
			::ScreenToClient(m_hWndHost, reinterpret_cast<LPPOINT>(&rcAnchorClient) + 1);
			using enum EAnchorSide;

			if (m_fSplitting) {
				const auto iOffsetX = iX - m_ptCurr.x;
				const auto iOffsetY = iY - m_ptCurr.y;
				bool fAllowSplit { };

				switch (m_eAnchorSide) {
				case SIDE_LEFT:
					rcAnchorClient.left += iOffsetX;
					fAllowSplit = (rcAnchorClient.left >= m_iMinEdge) && (rcAnchorClient.left <= m_iMaxEdge);
					break;
				case SIDE_TOP:
					rcAnchorClient.top += iOffsetY;
					fAllowSplit = (rcAnchorClient.top >= m_iMinEdge) && (rcAnchorClient.top <= m_iMaxEdge);
					break;
				case SIDE_RIGHT:
					rcAnchorClient.right += iOffsetX;
					fAllowSplit = (rcAnchorClient.right >= m_iMinEdge) && (rcAnchorClient.right <= m_iMaxEdge);
					break;
				case SIDE_BOTTOM:
					rcAnchorClient.bottom += iOffsetY;
					fAllowSplit = (rcAnchorClient.bottom >= m_iMinEdge) && (rcAnchorClient.bottom <= m_iMaxEdge);
					break;
				default:
					break;
				}

				if (fAllowSplit) {
					auto hdwp = ::BeginDeferWindowPos(static_cast<int>(m_vecItems.size() + 1)); //+1 is the m_hWndAnchor itself.
					hdwp = ::DeferWindowPos(hdwp, m_hWndAnchor, nullptr, rcAnchorClient.left, rcAnchorClient.top,
						rcAnchorClient.Width(), rcAnchorClient.Height(), SWP_NOACTIVATE | SWP_NOZORDER);

					for (const auto& [hWnd, fIsResize] : m_vecItems) {
						CRect rcWnd;
						::GetWindowRect(hWnd, &rcWnd);
						::ScreenToClient(m_hWndHost, reinterpret_cast<LPPOINT>(&rcWnd));
						::ScreenToClient(m_hWndHost, reinterpret_cast<LPPOINT>(&rcWnd) + 1);

						switch (m_eAnchorSide) {
						case SIDE_LEFT:
							if (fIsResize) {
								rcWnd.right += iOffsetX;
							}
							else {
								rcWnd.OffsetRect(iOffsetX, 0);
							}
							break;
						case SIDE_TOP:
							if (fIsResize) {
								rcWnd.bottom += iOffsetY;
							}
							else {
								rcWnd.OffsetRect(0, iOffsetY);
							}
							break;
						case SIDE_RIGHT:
							if (fIsResize) {
								rcWnd.left += iOffsetX;
							}
							else {
								rcWnd.OffsetRect(iOffsetX, 0);
							}
							break;
						case SIDE_BOTTOM:
							if (fIsResize) {
								rcWnd.top += iOffsetY;
							}
							else {
								rcWnd.OffsetRect(0, iOffsetY);
							}
							break;
						default:
							break;
						}

						hdwp = ::DeferWindowPos(hdwp, hWnd, nullptr, rcWnd.left, rcWnd.top,
									rcWnd.Width(), rcWnd.Height(), SWP_NOACTIVATE | SWP_NOZORDER);
					}

					m_ptCurr = POINT(iX, iY);
					::EndDeferWindowPos(hdwp);
				}
			}
			else {
				CRect rcSplitter;
				bool fCursorWE { };
				switch (m_eAnchorSide) {
				case SIDE_LEFT:
					rcSplitter.SetRect(rcAnchorClient.left - m_u32WidthHalf, rcAnchorClient.top,
					rcAnchorClient.left + m_u32WidthHalf, rcAnchorClient.bottom);
					fCursorWE = true;
					break;
				case SIDE_TOP:
					rcSplitter.SetRect(rcAnchorClient.left, rcAnchorClient.top - m_u32WidthHalf,
						rcAnchorClient.right, rcAnchorClient.top + m_u32WidthHalf);
					fCursorWE = false;
					break;
				case SIDE_RIGHT:
					rcSplitter.SetRect(rcAnchorClient.right - m_u32WidthHalf, rcAnchorClient.top,
						rcAnchorClient.right + m_u32WidthHalf, rcAnchorClient.bottom);
					fCursorWE = true;
					break;
				case SIDE_BOTTOM:
					rcSplitter.SetRect(rcAnchorClient.left, rcAnchorClient.bottom - m_u32WidthHalf,
						rcAnchorClient.right, rcAnchorClient.bottom + m_u32WidthHalf);
					fCursorWE = false;
					break;
				default:
					break;
				}

				if (rcSplitter.PtInRect(pt)) {
					static const auto hCurResizeWE =
						static_cast<HCURSOR>(::LoadImageW(nullptr, IDC_SIZEWE, IMAGE_CURSOR, 0, 0, LR_SHARED));
					static const auto hCurResizeNS =
						static_cast<HCURSOR>(::LoadImageW(nullptr, IDC_SIZENS, IMAGE_CURSOR, 0, 0, LR_SHARED));
					m_fCurInSplitter = true;

					//The cursor must be set here and not in the WM_SETCURSOR handler,
					//because the WM_SETCURSOR message doesn't fire when in SetCapture mode.
					::SetCursor(fCursorWE ? hCurResizeWE : hCurResizeNS);
					::SetCapture(m_hWndHost);
				}
				else {
					if (m_fCurInSplitter) {
						m_fCurInSplitter = false;
						::ReleaseCapture();
					}
				}
			}
		}

		//Private methods.

		bool CSplitter::IsLock()const {
			//Locking mechanism is needed to avoid Set/ReleaseCapture interference 
			//between two or more splitters in the same window.
			return m_pSplitterCurrentlyInUse != nullptr && m_pSplitterCurrentlyInUse != this;
		}

		void CSplitter::Lock() {
			m_pSplitterCurrentlyInUse = this;
		}

		void CSplitter::Unlock() {
			m_pSplitterCurrentlyInUse = nullptr;
		}

		class CDynLayout final {
		public:
			//Ratio settings, for how much to move or to resize child item when parent is resized.
			struct ItemRatio {
				[[nodiscard]] bool IsNull()const { return flXRatio == 0.F && flYRatio == 0.F; };
				float flXRatio { }; float flYRatio { };
			};
			struct MoveRatio : public ItemRatio { }; //To differentiate move from size in the AddItem.
			struct SizeRatio : public ItemRatio { };

			CDynLayout() = default;
			CDynLayout(HWND hWndHost) : m_hWndHost(hWndHost) { }
			void AddItem(int iItemID, MoveRatio move, SizeRatio size);
			void AddItem(HWND hWndItem, MoveRatio move, SizeRatio size);
			void Enable(bool fTrack);
			bool LoadFromResource(HINSTANCE hInstRes, const wchar_t* pwszResName);
			bool LoadFromResource(HINSTANCE hInstRes, UINT uResID);
			void WMSize(int iWidth, int iHeight)const; //Should be hooked into the host window's WM_SIZE handler.
			void RemoveAll() { m_vecItems.clear(); }
			void SetHost(HWND hWnd) { assert(hWnd != nullptr); m_hWndHost = hWnd; }
			void UpdateItem(int iItemID, MoveRatio move, SizeRatio size);
			void UpdateItem(HWND hWndItem, MoveRatio move, SizeRatio size);

			//Static helper methods to use in the AddItem.
			[[nodiscard]] static MoveRatio MoveNone() { return { }; }
			[[nodiscard]] static MoveRatio MoveHorz(int iXRatio) {
				return { { .flXRatio { ToFlRatio(iXRatio) } } };
			}
			[[nodiscard]] static MoveRatio MoveVert(int iYRatio) {
				return { { .flYRatio { ToFlRatio(iYRatio) } } };
			}
			[[nodiscard]] static MoveRatio MoveHorzAndVert(int iXRatio, int iYRatio) {
				return { { .flXRatio { ToFlRatio(iXRatio) }, .flYRatio { ToFlRatio(iYRatio) } } };
			}
			[[nodiscard]] static SizeRatio SizeNone() { return { }; }
			[[nodiscard]] static SizeRatio SizeHorz(int iXRatio) {
				return { { .flXRatio { ToFlRatio(iXRatio) } } };
			}
			[[nodiscard]] static SizeRatio SizeVert(int iYRatio) {
				return { { .flYRatio { ToFlRatio(iYRatio) } } };
			}
			[[nodiscard]] static SizeRatio SizeHorzAndVert(int iXRatio, int iYRatio) {
				return { { .flXRatio { ToFlRatio(iXRatio) }, .flYRatio { ToFlRatio(iYRatio) } } };
			}
		private:
			[[nodiscard]] static auto ToFlRatio(int iRatio) -> float {
				return std::clamp(iRatio, 0, 100) / 100.F;
			}
			struct ItemData {
				HWND hWnd { };   //Item window.
				RECT rcOrig { }; //Item original window rect after EnableTrack(true).
				MoveRatio move;  //How much to move the item.
				SizeRatio size;  //How much to resize the item.
			};
			HWND m_hWndHost { };   //Host window.
			RECT m_rcHostOrig { }; //Host original client area rect after EnableTrack(true).
			std::vector<ItemData> m_vecItems; //All items to resize/move.
			bool m_fTrack { };
		};

		void CDynLayout::AddItem(int iItemID, MoveRatio move, SizeRatio size) {
			AddItem(::GetDlgItem(m_hWndHost, iItemID), move, size);
		}

		void CDynLayout::AddItem(HWND hWndItem, MoveRatio move, SizeRatio size) {
			assert(hWndItem != nullptr);
			if (hWndItem == nullptr)
				return;

			m_vecItems.emplace_back(ItemData { .hWnd { hWndItem }, .move { move }, .size { size } });
		}

		void CDynLayout::Enable(bool fTrack) {
			m_fTrack = fTrack;
			if (m_fTrack) {
				::GetClientRect(m_hWndHost, &m_rcHostOrig);
				for (auto& [hWnd, rc, move, size] : m_vecItems) {
					::GetWindowRect(hWnd, &rc);
					::ScreenToClient(m_hWndHost, reinterpret_cast<LPPOINT>(&rc));
					::ScreenToClient(m_hWndHost, reinterpret_cast<LPPOINT>(&rc) + 1);
				}
			}
		}

		bool CDynLayout::LoadFromResource(HINSTANCE hInstRes, const wchar_t* pwszResName) {
			assert(pwszResName != nullptr);
			if (pwszResName == nullptr)
				return false;

			assert(m_hWndHost != nullptr);
			if (m_hWndHost == nullptr)
				return false;

			const auto hDlgLayout = ::FindResourceW(hInstRes, pwszResName, L"AFX_DIALOG_LAYOUT");
			if (hDlgLayout == nullptr) { //No such resource found in the hInstRes.
				return false;
			}

			const auto hResData = ::LoadResource(hInstRes, hDlgLayout);
			assert(hResData != nullptr);
			if (hResData == nullptr)
				return false;

			const auto pResData = ::LockResource(hResData);
			assert(pResData != nullptr);
			if (pResData == nullptr)
				return false;

			const auto dwSizeRes = ::SizeofResource(hInstRes, hDlgLayout);
			const auto* pDataBegin = reinterpret_cast<WORD*>(pResData);
			const auto* const pDataEnd = reinterpret_cast<WORD*>(reinterpret_cast<std::byte*>(pResData) + dwSizeRes);

			assert(*pDataBegin == 0);
			if (*pDataBegin != 0) //First WORD must be zero, it's a header (version number).
				return false;

			++pDataBegin; //Past first WORD is the actual data.
			auto hWndChild = ::GetWindow(m_hWndHost, GW_CHILD); //First child window in the host window.
			while (pDataBegin + 4 <= pDataEnd) { //Actual AFX_DIALOG_LAYOUT data.
				if (hWndChild == nullptr)
					break;

				const auto wXMoveRatio = *pDataBegin++;
				const auto wYMoveRatio = *pDataBegin++;
				const auto wXSizeRatio = *pDataBegin++;
				const auto wYSizeRatio = *pDataBegin++;
				AddItem(hWndChild, MoveHorzAndVert(wXMoveRatio, wYMoveRatio), SizeHorzAndVert(wXSizeRatio, wYSizeRatio));
				hWndChild = ::GetWindow(hWndChild, GW_HWNDNEXT);
			}

			return true;
		}

		bool CDynLayout::LoadFromResource(HINSTANCE hInstRes, UINT uResID) {
			return LoadFromResource(hInstRes, MAKEINTRESOURCEW(uResID));
		}

		void CDynLayout::WMSize(int iWidth, int iHeight)const {
			if (!m_fTrack)
				return;

			const auto iHostWidth = m_rcHostOrig.right - m_rcHostOrig.left;
			const auto iHostHeight = m_rcHostOrig.bottom - m_rcHostOrig.top;
			const auto iDeltaX = iWidth - iHostWidth;
			const auto iDeltaY = iHeight - iHostHeight;

			const auto hDWP = ::BeginDeferWindowPos(static_cast<int>(m_vecItems.size()));
			for (const auto& [hWnd, rc, move, size] : m_vecItems) {
				const auto iNewLeft = static_cast<int>(rc.left + (iDeltaX * move.flXRatio));
				const auto iNewTop = static_cast<int>(rc.top + (iDeltaY * move.flYRatio));
				const auto iOrigWidth = rc.right - rc.left;
				const auto iOrigHeight = rc.bottom - rc.top;
				const auto iNewWidth = static_cast<int>(iOrigWidth + (iDeltaX * size.flXRatio));
				const auto iNewHeight = static_cast<int>(iOrigHeight + (iDeltaY * size.flYRatio));
				::DeferWindowPos(hDWP, hWnd, nullptr, iNewLeft, iNewTop, iNewWidth, iNewHeight, SWP_NOZORDER | SWP_NOACTIVATE);
			}
			::EndDeferWindowPos(hDWP);
		}

		void CDynLayout::UpdateItem(int iItemID, MoveRatio move, SizeRatio size) {
			UpdateItem(::GetDlgItem(m_hWndHost, iItemID), move, size);
		}

		void CDynLayout::UpdateItem(HWND hWndItem, MoveRatio move, SizeRatio size) {
			assert(hWndItem != nullptr);
			if (hWndItem == nullptr)
				return;

			auto it = std::ranges::find_if(m_vecItems, [=](const ItemData& id) {
				return id.hWnd == hWndItem; });
			assert(it != m_vecItems.end());
			if (it != m_vecItems.end()) {
				it->move = move;
				it->size = size;
			}
		}

		class CWnd {
		public:
			CWnd() = default;
			CWnd(HWND hWnd) { Attach(hWnd); }
			~CWnd() = default;
			CWnd& operator=(HWND rhs) { Attach(rhs); return *this; };
			CWnd& operator=(CWnd rhs) { Attach(rhs); return *this; };
			operator HWND()const { return m_hWnd; }
			[[nodiscard]] bool operator==(CWnd rhs)const { return m_hWnd == rhs.m_hWnd; }
			[[nodiscard]] bool operator==(HWND rhs)const { return m_hWnd == rhs; }
			void Attach(HWND hWnd) { m_hWnd = hWnd; } //Can attach to nullptr as well.
			void CheckRadioButton(int iIDFirst, int iIDLast, int iIDCheck)const {
				assert(IsWindow()); ::CheckRadioButton(m_hWnd, iIDFirst, iIDLast, iIDCheck);
			}
			[[nodiscard]] auto ChildWindowFromPoint(POINT pt)const -> CWnd {
				assert(IsWindow()); return ::ChildWindowFromPoint(m_hWnd, pt);
			}
			void ClientToScreen(LPPOINT pPT)const { assert(IsWindow()); ::ClientToScreen(m_hWnd, pPT); }
			void ClientToScreen(LPRECT pRC)const {
				ClientToScreen(reinterpret_cast<LPPOINT>(pRC)); ClientToScreen((reinterpret_cast<LPPOINT>(pRC)) + 1);
			}
			void DestroyWindow() { assert(IsWindow()); ::DestroyWindow(m_hWnd); m_hWnd = nullptr; }
			void Detach() { m_hWnd = nullptr; }
			[[nodiscard]] auto GetClientRect()const -> CRect {
				assert(IsWindow()); RECT rc; ::GetClientRect(m_hWnd, &rc); return rc;
			}
			bool EnableWindow(bool fEnable)const { assert(IsWindow()); return ::EnableWindow(m_hWnd, fEnable); }
			void EndDialog(INT_PTR iResult)const { assert(IsWindow()); ::EndDialog(m_hWnd, iResult); }
			[[nodiscard]] int GetCheckedRadioButton(int iIDFirst, int iIDLast)const {
				assert(IsWindow()); for (int iID = iIDFirst; iID <= iIDLast; ++iID) {
					if (::IsDlgButtonChecked(m_hWnd, iID) != 0) { return iID; }
				} return 0;
			}
			[[nodiscard]] auto GetDC()const -> HDC { assert(IsWindow()); return ::GetDC(m_hWnd); }
			[[nodiscard]] int GetDlgCtrlID()const { assert(IsWindow()); return ::GetDlgCtrlID(m_hWnd); }
			[[nodiscard]] auto GetDlgItem(int iIDCtrl)const -> CWnd { assert(IsWindow()); return ::GetDlgItem(m_hWnd, iIDCtrl); }
			[[nodiscard]] auto GetHFont()const -> HFONT {
				assert(IsWindow()); return reinterpret_cast<HFONT>(SendMsg(WM_GETFONT, 0, 0));
			}
			[[nodiscard]] auto GetHWND()const -> HWND { return m_hWnd; }
			[[nodiscard]] auto GetLogFont()const -> std::optional<LOGFONTW> {
				if (const auto hFont = GetHFont(); hFont != nullptr) {
					LOGFONTW lf { }; ::GetObjectW(hFont, sizeof(lf), &lf); return lf;
				}
				return std::nullopt;
			}
			[[nodiscard]] auto GetParent()const -> CWnd { assert(IsWindow()); return ::GetParent(m_hWnd); }
			[[nodiscard]] auto GetScrollInfo(bool fVert, UINT uMask = SIF_ALL)const -> SCROLLINFO {
				assert(IsWindow()); SCROLLINFO si { .cbSize { sizeof(SCROLLINFO) }, .fMask { uMask } };
				::GetScrollInfo(m_hWnd, fVert, &si); return si;
			}
			[[nodiscard]] auto GetScrollPos(bool fVert)const -> int { return GetScrollInfo(fVert, SIF_POS).nPos; }
			[[nodiscard]] auto GetWindowDC()const -> HDC { assert(IsWindow()); return ::GetWindowDC(m_hWnd); }
			[[nodiscard]] auto GetWindowLongPTR(int iIndex)const {
				assert(IsWindow()); return ::GetWindowLongPtrW(m_hWnd, iIndex);
			}
			[[nodiscard]] auto GetWindowRect()const -> CRect {
				assert(IsWindow()); RECT rc; ::GetWindowRect(m_hWnd, &rc); return rc;
			}
			[[nodiscard]] auto GetWindowStyles()const { return static_cast<DWORD>(GetWindowLongPTR(GWL_STYLE)); }
			[[nodiscard]] auto GetWindowStylesEx()const { return static_cast<DWORD>(GetWindowLongPTR(GWL_EXSTYLE)); }
			[[nodiscard]] auto GetWndText()const -> std::wstring {
				assert(IsWindow()); wchar_t buff[256]; ::GetWindowTextW(m_hWnd, buff, std::size(buff)); return buff;
			}
			[[nodiscard]] auto GetWndTextSize()const -> DWORD { assert(IsWindow()); return ::GetWindowTextLengthW(m_hWnd); }
			void Invalidate(bool fErase)const { assert(IsWindow()); ::InvalidateRect(m_hWnd, nullptr, fErase); }
			[[nodiscard]] bool IsDlgMessage(MSG* pMsg)const { return ::IsDialogMessageW(m_hWnd, pMsg); }
			[[nodiscard]] bool IsNull()const { return m_hWnd == nullptr; }
			[[nodiscard]] bool IsWindow()const { return ::IsWindow(m_hWnd); }
			[[nodiscard]] bool IsWindowEnabled()const { assert(IsWindow()); return ::IsWindowEnabled(m_hWnd); }
			[[nodiscard]] bool IsWindowVisible()const { assert(IsWindow()); return ::IsWindowVisible(m_hWnd); }
			[[nodiscard]] bool IsWndTextEmpty()const { return GetWndTextSize() == 0; }
			void KillTimer(UINT_PTR uID)const {
				assert(IsWindow()); ::KillTimer(m_hWnd, uID);
			}
			int MapWindowPoints(HWND hWndTo, LPRECT pRC)const {
				assert(IsWindow()); return ::MapWindowPoints(m_hWnd, hWndTo, reinterpret_cast<LPPOINT>(pRC), 2);
			}
			bool RedrawWindow(LPCRECT pRC = nullptr, HRGN hrgn = nullptr, UINT uFlags = RDW_INVALIDATE | RDW_UPDATENOW | RDW_ERASE)const {
				assert(IsWindow()); return static_cast<bool>(::RedrawWindow(m_hWnd, pRC, hrgn, uFlags));
			}
			int ReleaseDC(HDC hDC)const { assert(IsWindow()); return ::ReleaseDC(m_hWnd, hDC); }
			void ScreenToClient(LPPOINT pPT)const { assert(IsWindow()); ::ScreenToClient(m_hWnd, pPT); }
			void ScreenToClient(POINT& pt)const { ScreenToClient(&pt); }
			void ScreenToClient(LPRECT pRC)const {
				ScreenToClient(reinterpret_cast<LPPOINT>(pRC)); ScreenToClient(reinterpret_cast<LPPOINT>(pRC) + 1);
			}
			auto SendMsg(UINT uMsg, WPARAM wParam = 0, LPARAM lParam = 0)const -> LRESULT {
				assert(IsWindow()); return ::SendMessageW(m_hWnd, uMsg, wParam, lParam);
			}
			void SetActiveWindow()const { assert(IsWindow()); ::SetActiveWindow(m_hWnd); }
			auto SetCapture()const -> CWnd { assert(IsWindow()); return ::SetCapture(m_hWnd); }
			auto SetClassLongPTR(int iIndex, LONG_PTR dwNewLong)const -> ULONG_PTR {
				assert(IsWindow()); return ::SetClassLongPtrW(m_hWnd, iIndex, dwNewLong);
			}
			void SetFocus()const { assert(IsWindow()); ::SetFocus(m_hWnd); }
			void SetForegroundWindow()const { assert(IsWindow()); ::SetForegroundWindow(m_hWnd); }
			void SetScrollInfo(bool fVert, const SCROLLINFO& si)const {
				assert(IsWindow()); ::SetScrollInfo(m_hWnd, fVert, &si, TRUE);
			}
			void SetScrollPos(bool fVert, int iPos)const {
				const SCROLLINFO si { .cbSize { sizeof(SCROLLINFO) }, .fMask { SIF_POS }, .nPos { iPos } };
				SetScrollInfo(fVert, si);
			}
			auto SetTimer(UINT_PTR uID, UINT uElapse, TIMERPROC pFN = nullptr)const -> UINT_PTR {
				assert(IsWindow()); return ::SetTimer(m_hWnd, uID, uElapse, pFN);
			}
			void SetWindowPos(HWND hWndAfter, int iX, int iY, int iWidth, int iHeight, UINT uFlags)const {
				assert(IsWindow()); ::SetWindowPos(m_hWnd, hWndAfter, iX, iY, iWidth, iHeight, uFlags);
			}
			auto SetWndClassLong(int iIndex, LONG_PTR dwNewLong)const -> ULONG_PTR {
				assert(IsWindow()); return ::SetClassLongPtrW(m_hWnd, iIndex, dwNewLong);
			}
			void SetWndText(LPCWSTR pwszStr)const { assert(IsWindow()); ::SetWindowTextW(m_hWnd, pwszStr); }
			void SetWndText(const std::wstring& wstr)const { SetWndText(wstr.data()); }
			void SetRedraw(bool fRedraw)const { assert(IsWindow()); SendMsg(WM_SETREDRAW, fRedraw, 0); }
			bool ShowWindow(int iCmdShow)const { assert(IsWindow()); return ::ShowWindow(m_hWnd, iCmdShow); }
			[[nodiscard]] static auto GetFocus() -> CWnd { return ::GetFocus(); }
		protected:
			HWND m_hWnd { }; //Windows window handle.
		};

		class CWndBtn final : public CWnd {
		public:
			[[nodiscard]] bool IsChecked()const { assert(IsWindow()); return ::SendMessageW(m_hWnd, BM_GETCHECK, 0, 0); }
			void SetBitmap(HBITMAP hBmp)const
			{
				assert(IsWindow()); ::SendMessageW(m_hWnd, BM_SETIMAGE, IMAGE_BITMAP, reinterpret_cast<LPARAM>(hBmp));
			}
			void SetCheck(bool fCheck)const { assert(IsWindow()); ::SendMessageW(m_hWnd, BM_SETCHECK, fCheck, 0); }
		};

		class CWndEdit final : public CWnd {
		public:
			void SetCueBanner(LPCWSTR pwszText, bool fDrawIfFocus = false)const
			{
				assert(IsWindow());
				::SendMessageW((m_hWnd), EM_SETCUEBANNER, static_cast<WPARAM>(fDrawIfFocus), reinterpret_cast<LPARAM>(pwszText));
			}
		};

		class CWndCombo final : public CWnd {
		public:
			int AddString(const std::wstring& wstr)const { return AddString(wstr.data()); }
			int AddString(LPCWSTR pwszStr)const
			{
				assert(IsWindow());
				return static_cast<int>(::SendMessageW(m_hWnd, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(pwszStr)));
			}
			int DeleteString(int iIndex)const
			{
				assert(IsWindow()); return static_cast<int>(::SendMessageW(m_hWnd, CB_DELETESTRING, iIndex, 0));
			}
			[[nodiscard]] int FindStringExact(int iIndex, LPCWSTR pwszStr)const
			{
				assert(IsWindow());
				return static_cast<int>(::SendMessageW(m_hWnd, CB_FINDSTRINGEXACT, iIndex, reinterpret_cast<LPARAM>(pwszStr)));
			}
			[[nodiscard]] int GetCount()const
			{
				assert(IsWindow()); return static_cast<int>(::SendMessageW(m_hWnd, CB_GETCOUNT, 0, 0));
			}
			[[nodiscard]] int GetCurSel()const
			{
				assert(IsWindow()); return static_cast<int>(::SendMessageW(m_hWnd, CB_GETCURSEL, 0, 0));
			}
			[[nodiscard]] auto GetItemData(int iIndex)const -> DWORD_PTR
			{
				assert(IsWindow()); return ::SendMessageW(m_hWnd, CB_GETITEMDATA, iIndex, 0);
			}
			[[nodiscard]] bool HasString(LPCWSTR pwszStr)const { return FindStringExact(0, pwszStr) != CB_ERR; };
			[[nodiscard]] bool HasString(const std::wstring& wstr)const { return HasString(wstr.data()); };
			int InsertString(int iIndex, const std::wstring& wstr)const { return InsertString(iIndex, wstr.data()); }
			int InsertString(int iIndex, LPCWSTR pwszStr)const
			{
				assert(IsWindow());
				return static_cast<int>(::SendMessageW(m_hWnd, CB_INSERTSTRING, iIndex, reinterpret_cast<LPARAM>(pwszStr)));
			}
			void LimitText(int iMaxChars)const { assert(IsWindow()); ::SendMessageW(m_hWnd, CB_LIMITTEXT, iMaxChars, 0); }
			void ResetContent()const { assert(IsWindow()); ::SendMessageW(m_hWnd, CB_RESETCONTENT, 0, 0); }
			void SetCueBanner(const std::wstring& wstr)const { SetCueBanner(wstr.data()); }
			void SetCueBanner(LPCWSTR pwszText)const
			{
				assert(IsWindow()); ::SendMessageW(m_hWnd, CB_SETCUEBANNER, 0, reinterpret_cast<LPARAM>(pwszText));
			}
			void SetCurSel(int iIndex)const { assert(IsWindow()); ::SendMessageW(m_hWnd, CB_SETCURSEL, iIndex, 0); }
			void SetItemData(int iIndex, DWORD_PTR dwData)const
			{
				assert(IsWindow()); ::SendMessageW(m_hWnd, CB_SETITEMDATA, iIndex, static_cast<LPARAM>(dwData));
			}
		};
	};

	namespace DXUT {
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

		[[nodiscard]] auto D2DGetFactory() -> ID2D1Factory1* {
			static const comptr pD2DFactory1 = []() {
				ID2D1Factory1* pFactory1;
				::D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1),
					reinterpret_cast<void**>(&pFactory1));
				assert(pFactory1 != nullptr);
				return pFactory1;
				}();
			return pD2DFactory1;
		}

		[[nodiscard]] auto D3D11GetDevice() -> ID3D11Device* {
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
				assert(pDevice != nullptr);
				return pDevice;
				}();
			return pD3D11Device;
		}

		[[nodiscard]] auto DXGIGetDevice() -> IDXGIDevice1* {
			static const comptr pDXGIDevice1 = []() {
				IDXGIDevice1* pDevice1;
				D3D11GetDevice()->QueryInterface(&pDevice1);
				assert(pDevice1 != nullptr);
				return pDevice1;
				}();
			return pDXGIDevice1;
		}

		[[nodiscard]] auto DXGICreateSwapChainForHWND(HWND hWnd) -> IDXGISwapChain1* {
			assert(::IsWindow(hWnd));
			const DXGI_SWAP_CHAIN_DESC1 scd { .Width { 0 }, .Height { 0 }, .Format { DXGI_FORMAT_B8G8R8A8_UNORM },
				.Stereo { false }, .SampleDesc { .Count { 1 }, .Quality { 0 } },
				.BufferUsage { DXGI_USAGE_RENDER_TARGET_OUTPUT }, .BufferCount { 2 }, .Scaling { DXGI_SCALING_NONE },
				.SwapEffect { DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL }, .AlphaMode { DXGI_ALPHA_MODE_UNSPECIFIED }, .Flags { 0 } };
			comptr<IDXGIAdapter> pDXGIAdapter;
			DXGIGetDevice()->GetAdapter(pDXGIAdapter);
			assert(pDXGIAdapter != nullptr);
			if (pDXGIAdapter == nullptr) { return { }; }

			comptr<IDXGIFactory2> pDXGIFactory2;
			pDXGIAdapter->GetParent(__uuidof(**(pDXGIFactory2.get_addr())), pDXGIFactory2);
			assert(pDXGIFactory2);
			if (!pDXGIFactory2) { return { }; }

			IDXGISwapChain1* pDXGISwapChain1;
			pDXGIFactory2->CreateSwapChainForHwnd(D3D11GetDevice(), hWnd, &scd, nullptr, nullptr, &pDXGISwapChain1);
			assert(pDXGISwapChain1 != nullptr);
			return pDXGISwapChain1;
		}

		[[nodiscard]] auto D2DCreateDevice() -> ID2D1Device* {
			ID2D1Device* pD2DDevice;
			D2DGetFactory()->CreateDevice(DXGIGetDevice(), &pD2DDevice);
			assert(pD2DDevice != nullptr);
			return pD2DDevice;
		}

		[[nodiscard]] auto D2DCreateDeviceContext(ID2D1Device* pD2DDevice) -> ID2D1DeviceContext* {
			assert(pD2DDevice);
			ID2D1DeviceContext* pD2DDeviceContext;
			pD2DDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &pD2DDeviceContext);
			assert(pD2DDeviceContext != nullptr);
			return pD2DDeviceContext;
		}

		[[nodiscard]] auto D2DCreateBitmapFromDxgiSurface(ID2D1DeviceContext* pD2DDC, IDXGISwapChain1* pDXGISwapChain) -> ID2D1Bitmap1* {
			assert(pD2DDC != nullptr);
			assert(pDXGISwapChain != nullptr);
			comptr<IDXGISurface> pDXGISurface;
			pDXGISwapChain->GetBuffer(0, __uuidof(**(pDXGISurface.get_addr())), pDXGISurface);
			assert(pDXGISurface != nullptr);
			if (pDXGISurface == nullptr) { return { }; }

			ID2D1Bitmap1* pD2DBitmap1;
			const auto bp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
			   D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
			pD2DDC->CreateBitmapFromDxgiSurface(pDXGISurface, bp, &pD2DBitmap1);
			return pD2DBitmap1;
		}

		[[nodiscard]] auto DWGetFactory() -> IDWriteFactory3* {
			static const comptr pD2DWriteFactory = []() {
				IDWriteFactory3* pWriteFactory;
				::DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory3),
					reinterpret_cast<IUnknown**>(&pWriteFactory));
				assert(pWriteFactory != nullptr);
				return pWriteFactory;
				}();
			return pD2DWriteFactory;
		}

		[[nodiscard]] auto DWCreateTextFormat(const DWFONTINFO& fi) -> IDWriteTextFormat1* {
			comptr<IDWriteTextFormat> pTextFormat;
			DWGetFactory()->CreateTextFormat(fi.wstrFamilyName.data(), nullptr, fi.eWeight, fi.eStyle, fi.eStretch,
				fi.flSizeDIP, fi.wstrLocale.data(), pTextFormat);
			assert(pTextFormat != nullptr);
			if (pTextFormat == nullptr) { return { }; }

			IDWriteTextFormat1* pTextFormat1;
			pTextFormat->QueryInterface(&pTextFormat1);
			return pTextFormat1;
		}

		[[nodiscard]] auto DWCreateTextLayout(std::wstring_view wsv, IDWriteTextFormat1* pTextFormat, float flWidthMax,
			float flHeightMax) -> IDWriteTextLayout1* {
			assert(pTextFormat);
			comptr<IDWriteTextLayout> pTextLayout;
			DWGetFactory()->CreateTextLayout(wsv.data(), static_cast<UINT32>(wsv.size()), pTextFormat, flWidthMax,
				flHeightMax, pTextLayout);
			assert(pTextLayout != nullptr);
			if (pTextLayout == nullptr) { return { }; }

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
			bool                        fIsMonospaced { };
		};

		[[nodiscard]] auto DWGetSystemFonts(const wchar_t* pwszLocale = L"en-US") -> std::vector<DWFONTFAMILYINFO> {
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
			if (!pSysFontSet) { return { }; }

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

				bool fIsMonospaced { false };
				if (iCountFontFaces > 0) {
					comptr<IDWriteFontFaceReference> pFontFaceReference;
					pFamilyNameSet->GetFontFaceReference(0, pFontFaceReference);
					if (pFontFaceReference != nullptr) {
						comptr<IDWriteFontFace3> pFontFace3;
						pFontFaceReference->CreateFontFace(pFontFace3);
						if (pFontFace3 != nullptr) {
							fIsMonospaced = pFontFace3->IsMonospacedFont();
						}
					}
				}

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
					.vecFontFaceInfo { std::move(vecFontFaceInfo) }, .fIsMonospaced { fIsMonospaced } });
			}

			return vecFontInfo;
		}

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
			struct DRAWCONTEXT {
				ID2D1DeviceContext* pDeviceContext { };
				ID2D1Brush*         pBrushTextDef { }; //Default text brush.
			};
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
			auto DrawInlineObject([[maybe_unused]] void* pContext, [[maybe_unused]] FLOAT flBaseLineX, [[maybe_unused]] FLOAT flBaseLineY,
				[[maybe_unused]] IDWriteInlineObject* pInlineObject, [[maybe_unused]] BOOL fIsSideways, [[maybe_unused]] BOOL fIsRightToLeft,
				[[maybe_unused]] IUnknown* pEffect) -> HRESULT override {
				return E_NOTIMPL;
			}
			auto DrawStrikethrough([[maybe_unused]] void* pContext, FLOAT flBaseLineX, FLOAT flBaseLineY,
				[[maybe_unused]] const DWRITE_STRIKETHROUGH* pStrikeThrough, [[maybe_unused]] IUnknown* pEffect) -> HRESULT override {
				const auto flTop = flBaseLineY + pStrikeThrough->offset;
				m_context.pDeviceContext->DrawLine(D2D1::Point2F(flBaseLineX, flTop),
					D2D1::Point2F(flBaseLineX + pStrikeThrough->width, flTop), m_context.pBrushTextDef, pStrikeThrough->thickness);
				return S_OK;
			}
			auto DrawUnderline([[maybe_unused]] void* pContext, FLOAT flBaseLineX, FLOAT flBaseLineY,
				const DWRITE_UNDERLINE* pUnderline, [[maybe_unused]] IUnknown* pEffect) -> HRESULT override {
				const auto flTop = flBaseLineY + pUnderline->offset;
				m_context.pDeviceContext->DrawLine(D2D1::Point2F(flBaseLineX, flTop),
					D2D1::Point2F(flBaseLineX + pUnderline->width, flTop), m_context.pBrushTextDef, pUnderline->thickness);
				return S_OK;
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
			void SetDrawContext(const DRAWCONTEXT& context) { m_context = context; }
		private:
			DRAWCONTEXT m_context;
		};
	}

	constexpr auto MSG_ITEM_CHANGED { 0x1 };

	struct FONTDATA {
		std::wstring                  wstrString;
		const DXUT::DWFONTFAMILYINFO* pFamilyInfo { };
		const DXUT::DWFONTFACEINFO*   pFaceInfo { };
	};


	//class CDWFontChooseList.

	class CDWFontChooseList final {
	public:
		CDWFontChooseList();
		~CDWFontChooseList();
		void Create(HWND hWndParent, UINT uCtrlID);
		[[nodiscard]] auto GetSelectedIndex()const -> UINT;
		[[nodiscard]] auto ProcessMsg(const MSG& msg) -> LRESULT;
		void SetData(std::span<FONTDATA> spnFD);
	private:
		struct ITEMSINVIEW {
			UINT32 u32FirstItem { }; //Index in the vector.
			UINT32 u32TotalInView { };
			float flFirstItemCoordXDIP { };
			float flFirstItemCoordYDIP { };
		};
		template <typename T> requires std::is_arithmetic_v<T>
		[[nodiscard]] auto DIPFromPixels(T t)const -> float;
		void EnsureVisible(UINT32 u32Item)const;
		[[nodiscard]] auto GetDPIScale()const -> float;
		[[nodiscard]] auto GetItemsInView()const -> ITEMSINVIEW;
		[[nodiscard]] auto GetItemsTotal()const -> UINT32;
		[[nodiscard]] auto GetLineHeightPx()const -> int;
		[[nodiscard]] auto GetLineSpacingDIP()const -> float;
		[[nodiscard]] auto GetScrollPosPx()const -> int;
		void ItemHighlight(UINT32 u32Item);
		void ItemSelect(UINT32 u32Item, bool fChangeHighlight);
		void ItemSelectIncDec(UINT32 u32Items, bool fInc);
		[[nodiscard]] bool IsDataSet()const;
		[[nodiscard]] int PixelsFromDIP(float flDIP)const;
		void RecalcScroll();
		void ScrollLines(int iLines);
		auto WMKeyDown(const MSG& msg) -> LRESULT;
		auto WMLButtonDown(const MSG& msg) -> LRESULT;
		auto WMMouseMove(const MSG& msg) -> LRESULT;
		auto WMMouseWheel(const MSG& msg) -> LRESULT;
		auto WMPaint() -> LRESULT;
		auto WMSize(const MSG& msg) -> LRESULT;
		auto WMVScroll(const MSG& msg) -> LRESULT;
		auto WMDPIChangedAfterParent() -> LRESULT;
		static auto CALLBACK SubclassProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
			UINT_PTR uIDSubclass, DWORD_PTR dwRefData)->LRESULT;
	private:
		static constexpr auto m_pwszClassName { L"DWFontChooseFontEnum" };
		static constexpr auto m_flSizeFontMainDIP { 12.F };
		static constexpr auto m_flLineSpacingDIP { m_flSizeFontMainDIP * 2.F }; //Text line height.
		static constexpr auto m_flBaseLineDIP { m_flLineSpacingDIP * 0.7F };
		GDIUT::CWnd m_Wnd;
		GDIUT::CPoint m_ptMouseCurr; //Current mouse position, to avoid spurious WM_MOUSEMOVE msgs.
		DXUT::comptr<ID2D1Device> m_pD2DDevice;
		DXUT::comptr<ID2D1DeviceContext> m_pD2DDeviceContext;
		DXUT::comptr<IDXGISwapChain1> m_pDXGISwapChain;
		DXUT::comptr<ID2D1Bitmap1> m_pD2DTargetBitmap;
		DXUT::comptr<ID2D1SolidColorBrush> m_pD2DBrushBlack;
		DXUT::comptr<ID2D1SolidColorBrush> m_pD2DBrushGray;
		DXUT::comptr<ID2D1SolidColorBrush> m_pD2DBrushLightGray;
		DXUT::comptr<IDWriteTextFormat1> m_pDWTextFormatMain;
		std::span<FONTDATA> m_spnFD;
		float m_flDPIScale { }; //DPI scale factor for the window.
		UINT32 m_u32ItemSelected { };
		UINT32 m_u32ItemHighlighted { };
	};

	CDWFontChooseList::CDWFontChooseList()
	{
		if (WNDCLASSEXW wc { }; ::GetClassInfoExW(nullptr, m_pwszClassName, &wc) == FALSE) {
			wc.cbSize = sizeof(WNDCLASSEXW);
			wc.style = CS_GLOBALCLASS | CS_DBLCLKS | CS_HREDRAW | CS_VREDRAW;
			wc.lpfnWndProc = GDIUT::WndProc<CDWFontChooseList>;
			wc.hCursor = static_cast<HCURSOR>(::LoadImageW(nullptr, IDC_ARROW, IMAGE_CURSOR, 0, 0, LR_DEFAULTSIZE | LR_SHARED));
			wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
			wc.lpszClassName = m_pwszClassName;
			if (::RegisterClassExW(&wc) == 0) {
				ut::DBG_REPORT(L"RegisterClassExW failed.");
				return;
			}
		}
	}

	CDWFontChooseList::~CDWFontChooseList() {
		::UnregisterClassW(m_pwszClassName, nullptr);
	}

	void CDWFontChooseList::Create(HWND hWndParent, UINT uCtrlID)
	{
		const auto hWnd = ::GetDlgItem(hWndParent, uCtrlID);
		if (hWnd == nullptr) {
			ut::DBG_REPORT(L"GetDlgItem failed.");
			return;
		}

		if (::SetWindowSubclass(hWnd, SubclassProc, reinterpret_cast<UINT_PTR>(this), 0) == FALSE) {
			ut::DBG_REPORT(L"SubclassDlgItem failed.");
			return;
		}

		m_Wnd.Attach(hWnd);
		const auto flDPI = static_cast<float>(::GetDpiForWindow(m_Wnd));
		m_flDPIScale = flDPI / USER_DEFAULT_SCREEN_DPI;

		//Direct2D.
		m_pD2DDevice = DXUT::D2DCreateDevice();
		m_pD2DDeviceContext = DXUT::D2DCreateDeviceContext(m_pD2DDevice);
		m_pDXGISwapChain = DXUT::DXGICreateSwapChainForHWND(m_Wnd);
		m_pD2DTargetBitmap = DXUT::D2DCreateBitmapFromDxgiSurface(m_pD2DDeviceContext, m_pDXGISwapChain);
		m_pD2DDeviceContext->SetTarget(m_pD2DTargetBitmap);
		m_pD2DDeviceContext->SetDpi(flDPI, flDPI);
		m_pD2DDeviceContext->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
		m_pD2DDeviceContext->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
		m_pD2DDeviceContext->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Black), m_pD2DBrushBlack);
		m_pD2DDeviceContext->CreateSolidColorBrush(D2D1::ColorF(0.8F, 0.8F, 0.8F), m_pD2DBrushGray);
		m_pD2DDeviceContext->CreateSolidColorBrush(D2D1::ColorF(0.9F, 0.9F, 0.9F), m_pD2DBrushLightGray);

		//Text.
		m_pDWTextFormatMain = DXUT::DWCreateTextFormat({ .wstrFamilyName { L"Courier New" },
			.wstrLocale { L"en-us" }, .flSizeDIP { m_flSizeFontMainDIP } });
		m_pDWTextFormatMain->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
		m_pDWTextFormatMain->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, GetLineSpacingDIP(), m_flBaseLineDIP);
	}

	auto CDWFontChooseList::GetSelectedIndex()const->UINT
	{
		return m_u32ItemSelected;
	}

	auto CDWFontChooseList::ProcessMsg(const MSG& msg)->LRESULT
	{
		switch (msg.message) {
		case WM_DPICHANGED_AFTERPARENT: return WMDPIChangedAfterParent();
		case WM_ERASEBKGND: return TRUE;
		case WM_GETDLGCODE: return DLGC_WANTALLKEYS;
		case WM_KEYDOWN:
		case WM_SYSKEYDOWN: return WMKeyDown(msg);
		case WM_LBUTTONDOWN: return WMLButtonDown(msg);
		case WM_MOUSEMOVE: return WMMouseMove(msg);
		case WM_MOUSEWHEEL: return WMMouseWheel(msg);
		case WM_PAINT: return WMPaint();
		case WM_SIZE: return WMSize(msg);
		case WM_VSCROLL: return WMVScroll(msg);
		default: return GDIUT::DefWndProc(msg);
		}
	}

	void CDWFontChooseList::SetData(std::span<FONTDATA> spnFD)
	{
		assert(m_Wnd.IsWindow());
		m_spnFD = spnFD;
		m_u32ItemHighlighted = 0; //Highlight first line on data set.
		RecalcScroll();
		m_Wnd.RedrawWindow();

		if (IsDataSet()) {
			ItemSelect(0, true); //Select the very first item.
		}
	}


	//Private methods.

	template <typename T> requires std::is_arithmetic_v<T>
	auto CDWFontChooseList::DIPFromPixels(T t)const->float {
		return t / GetDPIScale();
	}

	void CDWFontChooseList::EnsureVisible(UINT32 u32Item)const
	{
		const auto iScrollYPx = GetScrollPosPx();
		const auto iLineHeightPx = GetLineHeightPx();
		const auto rcClient = m_Wnd.GetClientRect();
		const int iItemPos = u32Item * iLineHeightPx;

		int iPosScrollNew = 0xFFFFFFFFU;
		if (iItemPos < iScrollYPx) {
			iPosScrollNew = iItemPos;
		}
		else if (iItemPos >= (iScrollYPx + rcClient.Height())) {
			iPosScrollNew = iItemPos - rcClient.Height() + iLineHeightPx;
		}

		if (iPosScrollNew != 0xFFFFFFFFU) {
			m_Wnd.SetScrollPos(true, iPosScrollNew);
		}

		m_Wnd.RedrawWindow();
	}

	auto CDWFontChooseList::GetDPIScale()const->float {
		return m_flDPIScale;
	}

	auto CDWFontChooseList::GetItemsInView()const->ITEMSINVIEW
	{
		const auto iScrollYPx = GetScrollPosPx();
		const auto iLineHeightPx = GetLineHeightPx();
		const auto rcClient = m_Wnd.GetClientRect();
		const UINT32 u32FirstItem = iScrollYPx / iLineHeightPx;
		UINT32 u32ItemsInView = (rcClient.Height() / iLineHeightPx) + (rcClient.Height() % iLineHeightPx > 0 ? 1 : 0);
		if (const auto uzSizeMax = GetItemsTotal(); u32ItemsInView + u32FirstItem > uzSizeMax) {
			u32ItemsInView = static_cast<UINT32>(uzSizeMax - u32FirstItem);
		}

		return { .u32FirstItem { u32FirstItem }, .u32TotalInView { u32ItemsInView },
			.flFirstItemCoordXDIP { DIPFromPixels(rcClient.Width() - (rcClient.Width() / 3)) },
			.flFirstItemCoordYDIP { DIPFromPixels(iScrollYPx % iLineHeightPx) } };
	}

	auto CDWFontChooseList::GetItemsTotal()const->UINT32
	{
		if (!IsDataSet()) {
			return { };
		}

		return static_cast<UINT32>(m_spnFD.size());
	}

	auto CDWFontChooseList::GetLineHeightPx()const->int {
		return PixelsFromDIP(GetLineSpacingDIP());
	}

	auto CDWFontChooseList::GetLineSpacingDIP()const->float {
		return m_flLineSpacingDIP;
	}

	auto CDWFontChooseList::GetScrollPosPx()const->int {
		return m_Wnd.GetScrollPos(true);
	}

	void CDWFontChooseList::ItemHighlight(UINT32 u32Item)
	{
		if (!IsDataSet()) {
			return;
		}

		if (u32Item >= GetItemsTotal()) {
			u32Item = GetItemsTotal() - 1;
		}

		m_u32ItemHighlighted = u32Item;
		m_Wnd.RedrawWindow();
	}

	void CDWFontChooseList::ItemSelect(UINT32 u32Item, bool fChangeHighlight)
	{
		if (!IsDataSet()) {
			return;
		}

		if (u32Item >= GetItemsTotal()) {
			u32Item = GetItemsTotal() - 1;
		}

		m_u32ItemSelected = u32Item;
		if (fChangeHighlight) {
			m_u32ItemHighlighted = m_u32ItemSelected;
		}

		NMHDR hdr { .hwndFrom { m_Wnd }, .idFrom { static_cast<UINT_PTR>(m_Wnd.GetDlgCtrlID()) }, .code { MSG_ITEM_CHANGED } };
		m_Wnd.GetParent().SendMsg(WM_NOTIFY, 0, reinterpret_cast<LPARAM>(&hdr));
		m_Wnd.RedrawWindow();
	}

	void CDWFontChooseList::ItemSelectIncDec(UINT32 u32Items, bool fInc)
	{
		if (!IsDataSet()) {
			return;
		}

		UINT32 u32ItemToSelect;
		if (fInc) {
			u32ItemToSelect = m_u32ItemSelected + u32Items;
		}
		else {
			if (u32Items > m_u32ItemSelected) {
				u32ItemToSelect = 0;
			}
			else {
				u32ItemToSelect = m_u32ItemSelected - u32Items;
			}
		}

		ItemSelect(u32ItemToSelect, false);
		EnsureVisible(u32ItemToSelect);
	}

	bool CDWFontChooseList::IsDataSet()const {
		return !m_spnFD.empty();
	}

	auto CDWFontChooseList::PixelsFromDIP(float flDIP)const->int {
		return std::lround(flDIP * GetDPIScale());
	}

	void CDWFontChooseList::RecalcScroll()
	{
		if (!IsDataSet()) {
			return;
		}

		const auto iMax = static_cast<int>(GetLineHeightPx() * GetItemsTotal());
		auto si = m_Wnd.GetScrollInfo(true);
		si.nPos = (iMax > si.nPos) ? si.nPos : iMax;
		si.nMax = iMax;
		si.nPage = m_Wnd.GetClientRect().Height();
		m_Wnd.SetScrollInfo(true, si);
	}

	void CDWFontChooseList::ScrollLines(int iLines)
	{
		m_Wnd.SetScrollPos(true, GetScrollPosPx() + (iLines * GetLineHeightPx()));
		m_Wnd.RedrawWindow();
	}

	auto CDWFontChooseList::WMKeyDown(const MSG& msg)->LRESULT
	{
		const auto wVKey = LOWORD(msg.wParam); //Virtual-key code (both: WM_KEYDOWN/WM_SYSKEYDOWN).

		switch (wVKey) {
		case VK_UP:
			ItemSelectIncDec(1, false);
			break;
		case VK_DOWN:
			ItemSelectIncDec(1, true);
			break;
		case VK_PRIOR:
			ItemSelectIncDec(GetItemsInView().u32TotalInView, false);
			break;
		case VK_NEXT:
			ItemSelectIncDec(GetItemsInView().u32TotalInView, true);
			break;
		case VK_HOME:
			ItemSelectIncDec(GetItemsTotal(), false);
			break;
		case VK_END:
			ItemSelectIncDec(GetItemsTotal(), true);
			break;
		default:
			break;
		}

		return 0;
	}

	auto CDWFontChooseList::WMLButtonDown(const MSG& msg)->LRESULT
	{
		const POINT pt { .x { ut::GetXLPARAM(msg.lParam) }, .y { ut::GetYLPARAM(msg.lParam) } };
		m_Wnd.SetFocus();
		ItemSelect(static_cast<UINT32>((pt.y + GetScrollPosPx()) / GetLineHeightPx()), true);

		return 0;
	}

	auto CDWFontChooseList::WMMouseMove(const MSG& msg)->LRESULT
	{
		const POINT pt { .x { ut::GetXLPARAM(msg.lParam) }, .y { ut::GetYLPARAM(msg.lParam) } };
		if (m_ptMouseCurr == pt) {
			return 0;
		}

		m_ptMouseCurr = pt;
		ItemHighlight(static_cast<UINT32>((pt.y + GetScrollPosPx()) / GetLineHeightPx()));

		return 0;
	}

	auto CDWFontChooseList::WMMouseWheel(const MSG& msg)->LRESULT
	{
		const auto iDelta = GET_WHEEL_DELTA_WPARAM(msg.wParam);
		ScrollLines(iDelta > 0 ? -2 : 2);

		return 0;
	}

	auto CDWFontChooseList::WMPaint()->LRESULT
	{
		::ValidateRect(m_Wnd, nullptr);
		m_pD2DDeviceContext->BeginDraw();
		m_pD2DDeviceContext->Clear(D2D1::ColorF(D2D1::ColorF::White));
		if (!IsDataSet()) {
			m_pD2DDeviceContext->EndDraw();
			m_pDXGISwapChain->Present(0, 0);
			return 0;
		}

		const auto iScrollYDIP = DIPFromPixels(GetScrollPosPx());
		const auto rcClient = m_Wnd.GetClientRect();
		const auto liv = GetItemsInView();
		const auto flHighlightedBkY = (m_u32ItemHighlighted * GetLineSpacingDIP()) - iScrollYDIP;
		const auto rcHighlightedBk = D2D1::RectF(0, flHighlightedBkY, DIPFromPixels(rcClient.Width()),
			flHighlightedBkY + GetLineSpacingDIP());
		m_pD2DDeviceContext->DrawRectangle(rcHighlightedBk, m_pD2DBrushGray, 1); //Highlighted item.
		const auto flSelectedBkY = (GetSelectedIndex() * GetLineSpacingDIP()) - iScrollYDIP;
		const auto rcSelectedBk = D2D1::RectF(0, flSelectedBkY, DIPFromPixels(rcClient.Width()),
			flSelectedBkY + GetLineSpacingDIP());
		m_pD2DDeviceContext->FillRectangle(rcSelectedBk, m_pD2DBrushLightGray); //Selected item.

		for (auto uLine = 0U; uLine < liv.u32TotalInView; ++uLine) {
			const auto uCurrLine = liv.u32FirstItem + uLine;
			const auto& fd = m_spnFD[uCurrLine];
			const auto pFamilyInfo = fd.pFamilyInfo;
			const auto pFaceInfo = fd.pFaceInfo;
			const auto pTextLayoutFName = DXUT::DWCreateTextLayout(fd.wstrString, m_pDWTextFormatMain, 0, 0);
			const auto flY = (static_cast<float>(uLine) * GetLineSpacingDIP()) - liv.flFirstItemCoordYDIP;
			m_pD2DDeviceContext->DrawTextLayout(D2D1::Point2F(DIPFromPixels(10.F), flY), pTextLayoutFName, m_pD2DBrushBlack);

			//Second column.
			const DXUT::comptr pTextFormat = DXUT::DWCreateTextFormat({ .wstrFamilyName { pFamilyInfo->wstrFamilyName },
				.wstrLocale { pFamilyInfo->wstrLocale },
				.eWeight { static_cast<DWRITE_FONT_WEIGHT>(std::stoi(pFaceInfo->wstrWeight)) },
				.eStretch { static_cast<DWRITE_FONT_STRETCH>(std::stoi(pFaceInfo->wstrStretch)) },
				.eStyle { static_cast<DWRITE_FONT_STYLE>(std::stoi(pFaceInfo->wstrStyle)) },
				.flSizeDIP { m_flSizeFontMainDIP * 1.5F } });
			pTextFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
			pTextFormat->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, GetLineSpacingDIP(), m_flBaseLineDIP);
			const auto pTextLayoutSample = DXUT::DWCreateTextLayout(L"Sample", pTextFormat, 0, 0);
			m_pD2DDeviceContext->DrawTextLayout(D2D1::Point2F(liv.flFirstItemCoordXDIP, flY), pTextLayoutSample, m_pD2DBrushBlack);
		}

		m_pD2DDeviceContext->EndDraw();
		m_pDXGISwapChain->Present(0, 0);

		return 0;
	}

	auto CDWFontChooseList::WMSize(const MSG& msg)->LRESULT
	{
		if (!m_pDXGISwapChain) {
			return 0;
		}

		const auto wWidth = LOWORD(msg.lParam);
		const auto wHeight = HIWORD(msg.lParam);
		m_pD2DDeviceContext->SetTarget(nullptr);
		m_pD2DTargetBitmap.safe_release();
		m_pDXGISwapChain->ResizeBuffers(0, wWidth, wHeight, DXGI_FORMAT_UNKNOWN, 0);
		m_pD2DTargetBitmap = DXUT::D2DCreateBitmapFromDxgiSurface(m_pD2DDeviceContext, m_pDXGISwapChain);
		m_pD2DDeviceContext->SetTarget(m_pD2DTargetBitmap);
		RecalcScroll();

		return 0;
	}

	auto CDWFontChooseList::WMVScroll(const MSG& msg)->LRESULT
	{
		auto si = m_Wnd.GetScrollInfo(true);
		switch (LOWORD(msg.wParam)) {
		case SB_TOP:
			si.nPos = si.nMin;
			break;
		case SB_BOTTOM:
			si.nPos = si.nMax;
			break;
		case SB_LINEUP:
			si.nPos -= PixelsFromDIP(5);
			break;
		case SB_LINEDOWN:
			si.nPos += PixelsFromDIP(5);
			break;
		case SB_PAGEUP:
			si.nPos -= si.nPage;
			break;
		case SB_PAGEDOWN:
			si.nPos += si.nPage;
			break;
		case SB_THUMBTRACK:
			si.nPos = si.nTrackPos;
			break;
		default:
			break;
		}

		si.fMask = SIF_POS;
		m_Wnd.SetScrollInfo(true, si);
		m_Wnd.RedrawWindow();

		return 0;
	}

	auto CDWFontChooseList::WMDPIChangedAfterParent()->LRESULT
	{
		const auto flDPI = static_cast<float>(::GetDpiForWindow(m_Wnd));
		m_flDPIScale = flDPI / USER_DEFAULT_SCREEN_DPI;
		m_pD2DDeviceContext->SetDpi(flDPI, flDPI);
		RecalcScroll();

		return 0;
	}

	auto CDWFontChooseList::SubclassProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
		UINT_PTR uIDSubclass, [[maybe_unused]] DWORD_PTR dwRefData) -> LRESULT
	{
		if (uMsg == WM_NCDESTROY) {
			::RemoveWindowSubclass(hWnd, SubclassProc, uIDSubclass);
		}

		const auto pCtrl = reinterpret_cast<CDWFontChooseList*>(uIDSubclass);
		return pCtrl->ProcessMsg({ .hwnd { hWnd }, .message { uMsg }, .wParam { wParam }, .lParam { lParam } });
	}


	//class CDWFontChooseSampleText.
	constexpr auto WM_MOSEWHEELUPCTRL { 0x01U };
	constexpr auto WM_MOSEWHEELDOWNCTRL { 0x02U };

	class CDWFontChooseSampleText final {
	public:
		CDWFontChooseSampleText();
		~CDWFontChooseSampleText();
		void Create(HWND hWndParent, UINT uCtrlID);
		[[nodiscard]] auto ProcessMsg(const MSG& msg) -> LRESULT;
		void SetFontInfo(const DWFONTINFO& fi);
		void SetUnderline(bool fIsUnderline);
		void SetStrikethrough(bool fIsStrikethrough);
	private:
		void ClipboardCopy()const;
		void ClipboardCut();
		void ClipboardPaste();
		void CreateTextLayout();
		template <typename T> requires std::is_arithmetic_v<T>
		[[nodiscard]] auto DIPFromPixels(T t)const -> float;
		void EnsureVisible(UINT32 u32TextPos)const;
		[[nodiscard]] auto GetDataSize()const -> UINT32;
		[[nodiscard]] auto GetDPIScale()const -> float;
		[[nodiscard]] auto GetLineHeightDIP()const -> float;
		[[nodiscard]] auto GetLineHeightPx()const -> UINT32;
		void OnKeyLeft();
		void OnKeyRight();
		void OnKeyUp();
		void OnKeyDown();
		void OnKeyDelete();
		void OnKeyEnter();
		void OnKeyBackspace();
		void OnKeyHome();
		void OnKeyEnd();
		[[nodiscard]] int PixelsFromDIP(float flDIP)const;
		void RecalcScroll();
		void RemoveSelected();
		void SelectAll();
		auto WMChar(const MSG& msg) -> LRESULT;
		auto WMKeyDown(const MSG& msg) -> INT_PTR;
		auto WMLButtonDblClk(const MSG& msg) -> INT_PTR;
		auto WMLButtonDown(const MSG& msg) -> INT_PTR;
		auto WMLButtonUp(const MSG& msg) -> INT_PTR;
		auto WMMouseMove(const MSG& msg) -> INT_PTR;
		auto WMMouseWheel(const MSG& msg) -> INT_PTR;
		auto WMPaint() -> LRESULT;
		auto WMSetCursor(const MSG& msg) -> LRESULT;
		auto WMSize(const MSG& msg) -> LRESULT;
		auto WMVScroll(const MSG& msg) -> LRESULT;
		auto WMDPIChangedAfterParent() -> LRESULT;
		static auto CALLBACK SubclassProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
			UINT_PTR uIDSubclass, DWORD_PTR dwRefData)->LRESULT;
	private:
		static constexpr auto m_pwszClassName { L"DWFontChooseSampleText" };
		GDIUT::CWnd m_Wnd;
		DXUT::comptr<ID2D1Device> m_pD2DDevice;
		DXUT::comptr<ID2D1DeviceContext> m_pD2DDeviceContext;
		DXUT::comptr<IDXGISwapChain1> m_pDXGISwapChain;
		DXUT::comptr<ID2D1Bitmap1> m_pD2DTargetBitmap;
		DXUT::comptr<ID2D1SolidColorBrush> m_pD2DBrushBlack;
		DXUT::comptr<ID2D1SolidColorBrush> m_pD2DBrushBlue;
		DXUT::comptr<ID2D1SolidColorBrush> m_pD2DBrushWhite;
		DXUT::comptr<IDWriteTextFormat1> m_pTextFormat;
		DXUT::comptr<IDWriteTextLayout1> m_pLayoutData;
		DXUT::CDWriteTextRenderer m_D2DTextRenderer;
		DXUT::CTextEffect m_effSelection;
		std::wstring m_wstrData { L"The quick brown fox jumps over the lazy dog." };
		UINT32 m_u32CaretPos { 0xFFFFFFFFU };
		UINT32 m_u32SelClick { };
		UINT32 m_u32SelStart { };
		UINT32 m_u32SelSize { };
		float m_flDPIScale { }; //DPI scale factor for the window.
		float m_flLineHeightDIP { };
		bool m_fLMDown { };
		bool m_fIsUnderline { false };
		bool m_fIsStrikethrough { false };
	};

	CDWFontChooseSampleText::CDWFontChooseSampleText() {
		if (WNDCLASSEXW wc { }; ::GetClassInfoExW(nullptr, m_pwszClassName, &wc) == FALSE) {
			wc.cbSize = sizeof(WNDCLASSEXW);
			wc.style = CS_GLOBALCLASS | CS_DBLCLKS | CS_HREDRAW | CS_VREDRAW;
			wc.lpfnWndProc = GDIUT::WndProc<CDWFontChooseList>;
			wc.hCursor = static_cast<HCURSOR>(::LoadImageW(nullptr, IDC_ARROW, IMAGE_CURSOR, 0, 0, LR_DEFAULTSIZE | LR_SHARED));
			wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
			wc.lpszClassName = m_pwszClassName;
			if (::RegisterClassExW(&wc) == 0) {
				ut::DBG_REPORT(L"RegisterClassExW failed.");
				return;
			}
		}
	}

	CDWFontChooseSampleText::~CDWFontChooseSampleText() {
		::UnregisterClassW(m_pwszClassName, nullptr);
	}

	void CDWFontChooseSampleText::Create(HWND hWndParent, UINT uCtrlID)
	{
		const auto hWnd = ::GetDlgItem(hWndParent, uCtrlID);
		if (hWnd == nullptr) {
			ut::DBG_REPORT(L"GetDlgItem failed.");
			return;
		}

		if (::SetWindowSubclass(hWnd, SubclassProc, reinterpret_cast<UINT_PTR>(this), 0) == FALSE) {
			ut::DBG_REPORT(L"SubclassDlgItem failed.");
			return;
		}

		m_Wnd.Attach(hWnd);
		const auto flDPI = static_cast<float>(::GetDpiForWindow(m_Wnd));
		m_flDPIScale = flDPI / USER_DEFAULT_SCREEN_DPI;

		//Direct2D.
		m_pD2DDevice = DXUT::D2DCreateDevice();
		m_pD2DDeviceContext = DXUT::D2DCreateDeviceContext(m_pD2DDevice);
		m_pDXGISwapChain = DXUT::DXGICreateSwapChainForHWND(m_Wnd);
		m_pD2DTargetBitmap = DXUT::D2DCreateBitmapFromDxgiSurface(m_pD2DDeviceContext, m_pDXGISwapChain);
		m_pD2DDeviceContext->SetTarget(m_pD2DTargetBitmap);
		m_pD2DDeviceContext->SetDpi(flDPI, flDPI);
		m_pD2DDeviceContext->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
		m_pD2DDeviceContext->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
		m_pD2DDeviceContext->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Black), m_pD2DBrushBlack);
		m_pD2DDeviceContext->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Blue), m_pD2DBrushBlue);
		m_pD2DDeviceContext->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), m_pD2DBrushWhite);
		m_effSelection.SetBkBrush(m_pD2DBrushBlue);
		m_effSelection.SetTextBrush(m_pD2DBrushWhite);
	}

	auto CDWFontChooseSampleText::ProcessMsg(const MSG& msg)->LRESULT
	{
		switch (msg.message) {
		case WM_CHAR: return WMChar(msg);
		case WM_DPICHANGED_AFTERPARENT: return WMDPIChangedAfterParent();
		case WM_ERASEBKGND: return TRUE;
		case WM_GETDLGCODE: return DLGC_WANTALLKEYS;
		case WM_KEYDOWN: return WMKeyDown(msg);
		case WM_LBUTTONDBLCLK: return WMLButtonDblClk(msg);
		case WM_LBUTTONDOWN: return WMLButtonDown(msg);
		case WM_LBUTTONUP: return WMLButtonUp(msg);
		case WM_MOUSEMOVE: return WMMouseMove(msg);
		case WM_MOUSEWHEEL: return WMMouseWheel(msg);
		case WM_PAINT: return WMPaint();
		case WM_SETCURSOR: return WMSetCursor(msg);
		case WM_SIZE: return WMSize(msg);
		case WM_VSCROLL: return WMVScroll(msg);
		default: return GDIUT::DefWndProc(msg);
		}
	}

	void CDWFontChooseSampleText::SetFontInfo(const DWFONTINFO& fi)
	{
		m_pTextFormat = DXUT::DWCreateTextFormat(fi);
		CreateTextLayout();
		m_D2DTextRenderer.SetDrawContext({ .pDeviceContext { m_pD2DDeviceContext }, .pBrushTextDef { m_pD2DBrushBlack } });
	}

	void CDWFontChooseSampleText::SetUnderline(bool fIsUnderline)
	{
		m_fIsUnderline = fIsUnderline;
		m_Wnd.RedrawWindow();
	}

	void CDWFontChooseSampleText::SetStrikethrough(bool fIsStrikethrough)
	{
		m_fIsStrikethrough = fIsStrikethrough;
		m_Wnd.RedrawWindow();
	}

	//Private methods.

	void CDWFontChooseSampleText::ClipboardCopy()const
	{
		if (m_u32SelSize == 0)
			return;

		const auto wstr = m_wstrData.substr(m_u32SelStart, m_u32SelSize);
		const auto sSize = (wstr.size() + 1) * sizeof(wchar_t);
		const auto hMem = ::GlobalAlloc(GMEM_MOVEABLE, sSize);
		if (!hMem) {
			ut::DBG_REPORT(L"GlobalAlloc error.");
			return;
		}

		const auto pMemLock = ::GlobalLock(hMem);
		if (!pMemLock) {
			ut::DBG_REPORT(L"GlobalLock error.");
			return;
		}

		std::memcpy(pMemLock, wstr.data(), sSize);
		::GlobalUnlock(hMem);
		if (::OpenClipboard(m_Wnd) == FALSE) {
			ut::DBG_REPORT(L"OpenClipboard error.");
			return;
		}

		::EmptyClipboard();
		::SetClipboardData(CF_UNICODETEXT, hMem);
		::CloseClipboard();
	}

	void CDWFontChooseSampleText::ClipboardCut()
	{
		if (m_u32SelSize == 0)
			return;

		ClipboardCopy();
		RemoveSelected();
		CreateTextLayout();
	}

	void CDWFontChooseSampleText::ClipboardPaste()
	{
		if (!m_pLayoutData || !::OpenClipboard(m_Wnd))
			return;

		const auto hClpbrd = ::GetClipboardData(CF_UNICODETEXT);
		if (!hClpbrd) {
			ut::DBG_REPORT(L"GetClipboardData error.");
			return;
		}

		const auto pData = static_cast<wchar_t*>(::GlobalLock(hClpbrd));
		if (pData == nullptr) {
			ut::DBG_REPORT(L"GlobalLock error.");
			::CloseClipboard();
			return;
		}

		RemoveSelected();
		m_wstrData.insert(m_u32CaretPos, pData);
		::GlobalUnlock(hClpbrd);
		::CloseClipboard();
		CreateTextLayout();
	}

	void CDWFontChooseSampleText::CreateTextLayout()
	{
		if (GetDataSize() > 1024) { //Trim the string to 1024 wchars.
			m_wstrData.resize(1024);
		}

		const auto rcClient = m_Wnd.GetClientRect();
		m_pLayoutData = DXUT::DWCreateTextLayout(m_wstrData, m_pTextFormat,
			DIPFromPixels(rcClient.Width()), DIPFromPixels(rcClient.Height()));
		m_pLayoutData->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
		m_pLayoutData->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

		UINT32 u32Lines;
		m_pLayoutData->GetLineMetrics(nullptr, 0, &u32Lines);
		const auto lm = std::make_unique_for_overwrite<DWRITE_LINE_METRICS[]>(u32Lines);
		m_pLayoutData->GetLineMetrics(lm.get(), u32Lines, &u32Lines);
		m_flLineHeightDIP = lm[0].height;

		RecalcScroll();
		m_Wnd.RedrawWindow();
	}

	template <typename T> requires std::is_arithmetic_v<T>
	auto CDWFontChooseSampleText::DIPFromPixels(T t)const->float {
		return t / GetDPIScale();
	}

	void CDWFontChooseSampleText::EnsureVisible(UINT32 u32TextPos)const
	{
		const auto iScrollYPx = m_Wnd.GetScrollPos(true);
		const auto iHeightClnt = m_Wnd.GetClientRect().Height();

		DWRITE_HIT_TEST_METRICS htm;
		float flX;
		float flY;
		m_pLayoutData->HitTestTextPosition(u32TextPos, FALSE, &flX, &flY, &htm);
		const int iCurrPosTopPx = PixelsFromDIP(flY);
		const int iCurrPosBotPx = PixelsFromDIP(flY + htm.height);

		int iPosScrollNew = 0xFFFFFFFFU;
		if (iCurrPosTopPx < iScrollYPx) {
			iPosScrollNew = iCurrPosTopPx;
		}
		else if (iCurrPosTopPx >= (iScrollYPx + iHeightClnt)
			|| (iCurrPosBotPx >= (iScrollYPx + iHeightClnt) && (PixelsFromDIP(htm.height) <= iHeightClnt))) {
			iPosScrollNew = iCurrPosTopPx - iHeightClnt + PixelsFromDIP(htm.height);
		}

		if (iPosScrollNew != 0xFFFFFFFF) {
			m_Wnd.SetScrollPos(true, iPosScrollNew);
		}

		m_Wnd.RedrawWindow();
	}

	auto CDWFontChooseSampleText::GetDataSize()const->UINT32 {
		return static_cast<UINT32>(m_wstrData.size());
	}

	auto CDWFontChooseSampleText::GetDPIScale()const->float {
		return m_flDPIScale;
	}

	auto CDWFontChooseSampleText::GetLineHeightDIP()const->float {
		return m_flLineHeightDIP;
	}

	auto CDWFontChooseSampleText::GetLineHeightPx()const->UINT32 {
		return PixelsFromDIP(GetLineHeightDIP());
	}

	void CDWFontChooseSampleText::OnKeyLeft()
	{
		if (::GetAsyncKeyState(VK_SHIFT) < 0) {
			if (m_u32CaretPos > 0) {
				if (m_u32SelSize == 0) {
					m_u32SelClick = m_u32CaretPos;
					m_u32SelStart = m_u32CaretPos - 1;
					++m_u32SelSize;
				}
				else {
					if (m_u32CaretPos > m_u32SelClick) {
						--m_u32SelSize;
					}
					else {
						++m_u32SelSize;
						--m_u32SelStart;
					}
				}
				--m_u32CaretPos;
			}
		}
		else {
			if (m_u32SelSize == 0) {
				if (m_u32CaretPos > 0) {
					--m_u32CaretPos;
				}
			}
			else {
				m_u32CaretPos = m_u32SelStart;
			}

			m_u32SelSize = 0;
		}

		EnsureVisible(m_u32CaretPos);
	}

	void CDWFontChooseSampleText::OnKeyRight()
	{
		if (::GetAsyncKeyState(VK_SHIFT) < 0) {
			if (m_u32CaretPos < GetDataSize()) {
				if (m_u32SelSize == 0) {
					m_u32SelClick = m_u32SelStart = m_u32CaretPos;
					++m_u32SelSize;
				}
				else {
					if (m_u32CaretPos > m_u32SelStart) {
						++m_u32SelSize;
					}
					else {
						++m_u32SelStart;
						--m_u32SelSize;
					}
				}
				++m_u32CaretPos;
			}
		}
		else {
			if (m_u32SelSize == 0) {
				if (m_u32CaretPos < GetDataSize()) {
					++m_u32CaretPos;
				}
			}
			else {
				m_u32CaretPos = m_u32SelStart + m_u32SelSize;
			}

			m_u32SelSize = 0;
		}

		EnsureVisible(m_u32CaretPos);
	}

	void CDWFontChooseSampleText::OnKeyUp()
	{
		float flX;
		float flY;
		DWRITE_HIT_TEST_METRICS htm;
		BOOL fIsTrail;
		BOOL fIsInside;
		DWRITE_TEXT_METRICS tm;
		m_pLayoutData->GetMetrics(&tm);

		if (::GetAsyncKeyState(VK_SHIFT) < 0) {
			m_pLayoutData->HitTestTextPosition(m_u32CaretPos, FALSE, &flX, &flY, &htm);
			const auto u32LineSel = static_cast<UINT32>(std::lround((flY - tm.top) / GetLineHeightDIP()));
			if (m_u32SelSize == 0) {
				if (u32LineSel == 0) { //First line.
					m_u32SelClick = m_u32CaretPos;
					m_u32SelStart = 0;
					m_u32SelSize = m_u32SelClick;
					m_u32CaretPos = 0;
				}
				else {
					m_pLayoutData->HitTestPoint(flX, flY - GetLineHeightDIP(), &fIsTrail, &fIsInside, &htm);
					m_u32SelClick = m_u32CaretPos;
					m_u32SelStart = htm.textPosition;
					m_u32SelSize = m_u32SelClick - m_u32SelStart;
					m_u32CaretPos = m_u32SelStart;
				}
			}
			else { //There is a selection.
				if (u32LineSel == 0) { //First line.
					m_u32SelSize = m_u32SelClick;
					m_u32SelStart = 0;
					m_u32CaretPos = 0;
				}
				else {
					m_pLayoutData->HitTestPoint(flX, flY - GetLineHeightDIP(), &fIsTrail, &fIsInside, &htm);
					if (htm.textPosition < m_u32SelClick) {
						m_u32SelStart = htm.textPosition;
						m_u32SelSize = m_u32SelClick - htm.textPosition;
						m_u32CaretPos = m_u32SelStart;
					}
					else {
						m_u32SelSize = htm.textPosition - m_u32SelStart;
						m_u32CaretPos = m_u32SelStart + m_u32SelSize;
					}
				}
			}
		}
		else {
			m_pLayoutData->HitTestTextPosition(m_u32SelSize == 0 ? m_u32CaretPos : m_u32SelStart, FALSE, &flX, &flY, &htm);
			const auto u32LineSel = static_cast<UINT32>(std::lround((flY - tm.top) / GetLineHeightDIP()));
			if (u32LineSel == 0) { //First line.
				m_u32CaretPos = 0;
			}
			else {
				m_pLayoutData->HitTestPoint(flX, flY - GetLineHeightDIP(), &fIsTrail, &fIsInside, &htm);
				m_u32CaretPos = htm.textPosition;
			}

			m_u32SelSize = 0;
		}

		EnsureVisible(m_u32CaretPos);
	}

	void CDWFontChooseSampleText::OnKeyDown()
	{
		float flX;
		float flY;
		DWRITE_HIT_TEST_METRICS htm;
		BOOL fIsTrail;
		BOOL fIsInside;
		DWRITE_TEXT_METRICS tm;
		m_pLayoutData->GetMetrics(&tm);

		if (::GetAsyncKeyState(VK_SHIFT) < 0) {
			m_pLayoutData->HitTestTextPosition(m_u32CaretPos, FALSE, &flX, &flY, &htm);
			const auto u32LineCaret = static_cast<UINT32>(std::lround((flY - tm.top) / GetLineHeightDIP()));
			if (m_u32SelSize == 0) {
				if (u32LineCaret == (tm.lineCount - 1)) { //Last line.
					m_u32SelClick = m_u32SelStart = m_u32CaretPos;
					m_u32CaretPos = GetDataSize();
					m_u32SelSize = m_u32CaretPos - m_u32SelStart;
				}
				else {
					m_pLayoutData->HitTestPoint(flX, flY + GetLineHeightDIP() + (GetLineHeightDIP() / 2),
						&fIsTrail, &fIsInside, &htm);
					m_u32SelClick = m_u32SelStart = m_u32CaretPos;
					m_u32CaretPos = htm.textPosition;
					m_u32SelSize = m_u32CaretPos - m_u32SelStart;
				}
			}
			else {
				if (u32LineCaret == (tm.lineCount - 1)) { //Last line.
					m_u32SelStart = m_u32SelClick;
					m_u32CaretPos = GetDataSize();
					m_u32SelSize = m_u32CaretPos - m_u32SelStart;
				}
				else {
					m_pLayoutData->HitTestPoint(flX, flY + GetLineHeightDIP() + (GetLineHeightDIP() / 2),
						&fIsTrail, &fIsInside, &htm);
					if (htm.textPosition < m_u32SelClick) {
						m_u32SelStart = htm.textPosition;
						m_u32CaretPos = m_u32SelStart;
						m_u32SelSize = m_u32SelClick - m_u32SelStart;
					}
					else {
						m_u32SelStart = m_u32SelClick;
						m_u32CaretPos = htm.textPosition;
						m_u32SelSize = m_u32CaretPos - m_u32SelStart;
					}
				}
			}
		}
		else {
			m_pLayoutData->HitTestTextPosition(m_u32SelSize == 0 ? m_u32CaretPos : m_u32SelStart + m_u32SelSize,
				FALSE, &flX, &flY, &htm);
			const auto u32LineCaret = static_cast<UINT32>(std::lround((flY - tm.top) / GetLineHeightDIP()));
			if (u32LineCaret == (tm.lineCount - 1)) { //Last line.
				m_u32CaretPos = GetDataSize();
			}
			else {
				m_pLayoutData->HitTestPoint(flX, flY + GetLineHeightDIP() + (GetLineHeightDIP() / 2), &fIsTrail, &fIsInside, &htm);
				m_u32CaretPos = htm.textPosition + (fIsTrail ? 1 : 0);
			}

			m_u32SelSize = 0;
		}

		EnsureVisible(m_u32CaretPos);
	}

	void CDWFontChooseSampleText::OnKeyDelete()
	{
		if (GetDataSize() == 0) {
			return;
		}

		if (m_u32SelSize == 0) {
			m_wstrData.erase(m_u32CaretPos, 1);
		}
		else {
			RemoveSelected();
		}

		CreateTextLayout();
		EnsureVisible(m_u32CaretPos);
	}

	void CDWFontChooseSampleText::OnKeyEnter()
	{
		RemoveSelected();
		m_wstrData.insert(m_u32CaretPos++, 1, L'\r');
		CreateTextLayout();
		EnsureVisible(m_u32CaretPos);
	}

	void CDWFontChooseSampleText::OnKeyBackspace()
	{
		if (GetDataSize() == 0) {
			return;
		}

		if (m_u32SelSize == 0) {
			if (m_u32CaretPos == 0) {
				return;
			}

			m_wstrData.erase(--m_u32CaretPos, 1);
		}
		else {
			RemoveSelected();
		}

		CreateTextLayout();
		EnsureVisible(m_u32CaretPos);
	}

	void CDWFontChooseSampleText::OnKeyHome() {
		EnsureVisible(m_u32CaretPos = 0);
	}

	void CDWFontChooseSampleText::OnKeyEnd() {
		EnsureVisible(m_u32CaretPos = GetDataSize());
	}

	auto CDWFontChooseSampleText::PixelsFromDIP(float flDIP)const ->int {
		return std::lround(flDIP * GetDPIScale());
	}

	void CDWFontChooseSampleText::RecalcScroll()
	{
		if (m_pLayoutData == nullptr) {
			return;
		}

		//It's important to SetMaxWidth BEFORE GetMetrics, as this primarily affects the tm.height results.
		const auto rcClient = m_Wnd.GetClientRect();
		m_pLayoutData->SetMaxWidth(DIPFromPixels(rcClient.Width()));
		DWRITE_TEXT_METRICS tm;
		m_pLayoutData->GetMetrics(&tm);
		m_pLayoutData->SetMaxHeight(std::fmax(tm.height, DIPFromPixels(rcClient.Height())));
		const auto iMax = PixelsFromDIP(tm.height);
		auto si = m_Wnd.GetScrollInfo(true);
		si.nPos = (iMax > si.nPos) ? si.nPos : iMax;
		si.nMax = iMax;
		si.nPage = rcClient.Height();
		m_Wnd.SetScrollInfo(true, si);
	}

	void CDWFontChooseSampleText::RemoveSelected()
	{
		if (m_u32SelSize == 0)
			return;

		m_wstrData.erase(m_u32SelStart, m_u32SelSize);
		m_u32CaretPos = m_u32SelStart;
		m_u32SelStart = m_u32SelSize = 0;
	}

	void CDWFontChooseSampleText::SelectAll()
	{
		m_u32SelStart = 0;
		m_u32SelSize = GetDataSize();
		m_Wnd.RedrawWindow();
	}

	auto CDWFontChooseSampleText::WMChar(const MSG& msg)->LRESULT
	{
		const auto wChar = LOWORD(msg.wParam); //LOWORD contains wchar_t symbol.
		if ((::GetAsyncKeyState(VK_CONTROL) < 0) || !std::iswprint(wChar)) {
			return 0;
		}

		RemoveSelected();
		m_wstrData.insert(m_u32CaretPos++, 1, wChar);
		CreateTextLayout();

		return 0;
	}

	auto CDWFontChooseSampleText::WMKeyDown(const MSG& msg)->LRESULT
	{
		const auto wVKey = LOWORD(msg.wParam); //Virtual-key code (both: WM_KEYDOWN/WM_SYSKEYDOWN).

		switch (wVKey) {
		case VK_BACK:
			OnKeyBackspace();
			break;
		case VK_DELETE:
			OnKeyDelete();
			break;
		case VK_RETURN:
			OnKeyEnter();
			break;
		case VK_LEFT:
			OnKeyLeft();
			break;
		case VK_RIGHT:
			OnKeyRight();
			break;
		case VK_UP:
			OnKeyUp();
			break;
		case VK_DOWN:
			OnKeyDown();
			break;
		case VK_HOME:
			OnKeyHome();
			break;
		case VK_END:
			OnKeyEnd();
			break;
		default:
			break;
		}

		if (::GetAsyncKeyState(VK_CONTROL) < 0) { //Ctrl+...
			switch (wVKey) {
			case 'A':
				SelectAll();
				break;
			case 'C':
			case VK_INSERT:
				ClipboardCopy();
				break;
			case 'X':
				ClipboardCut();
				break;
			case 'V':
				ClipboardPaste();
				break;
			default:
				break;
			}
		}
		else if (::GetAsyncKeyState(VK_SHIFT) < 0) { //Shift+...
			switch (wVKey) {
			case VK_INSERT:
				ClipboardPaste();
				break;
			default:
				break;
			}
		}

		return 0;
	}

	auto CDWFontChooseSampleText::WMLButtonDblClk(const MSG& msg)->INT_PTR
	{
		if (!m_pLayoutData) { return 0; }

		const POINT pt { .x { ut::GetXLPARAM(msg.lParam) }, .y { ut::GetYLPARAM(msg.lParam) } };
		BOOL fIsTrail;
		BOOL fIsInside;
		DWRITE_HIT_TEST_METRICS htm;
		m_pLayoutData->HitTestPoint(DIPFromPixels(pt.x), DIPFromPixels(pt.y) + DIPFromPixels(m_Wnd.GetScrollPos(true)),
			&fIsTrail, &fIsInside, &htm);

		if (fIsInside) { //Select a clicked word.
			const std::wstring_view wsvBefore(m_wstrData.data(), htm.textPosition); //View beforehead.
			if (m_wstrData.at(htm.textPosition) == L' ') {
				m_u32SelStart = static_cast<UINT32>(wsvBefore.find_last_not_of(L' ') + 1);
				if (const auto uzNotSpace = m_wstrData.find_first_not_of(L' ', htm.textPosition);
					uzNotSpace == std::wstring::npos) {
					m_u32SelSize = GetDataSize() - m_u32SelStart;
				}
				else {
					m_u32SelSize = static_cast<UINT32>(uzNotSpace - m_u32SelStart);
				}
			}
			else {
				m_u32SelStart = static_cast<UINT32>(wsvBefore.find_last_of(L' ') + 1);
				if (const auto uzSpace = m_wstrData.find_first_of(L' ', htm.textPosition);
					uzSpace == std::wstring::npos) {
					m_u32SelSize = GetDataSize() - m_u32SelStart;
				}
				else {
					m_u32SelSize = static_cast<UINT32>(uzSpace - m_u32SelStart);
				}
			}

			m_u32CaretPos = m_u32SelStart + m_u32SelSize;
			m_Wnd.RedrawWindow();
		}

		return 0;
	}

	auto CDWFontChooseSampleText::WMLButtonDown([[maybe_unused]] const MSG& msg)->LRESULT
	{
		if (!m_pLayoutData) { return 0; }

		const POINT pt { .x { ut::GetXLPARAM(msg.lParam) }, .y { ut::GetYLPARAM(msg.lParam) } };
		BOOL fIsTrail;
		BOOL fIsInside;
		DWRITE_HIT_TEST_METRICS htm;
		m_pLayoutData->HitTestPoint(DIPFromPixels(pt.x), DIPFromPixels(pt.y) + DIPFromPixels(m_Wnd.GetScrollPos(true)),
			&fIsTrail, &fIsInside, &htm);
		m_u32CaretPos = m_u32SelClick = htm.textPosition + (fIsTrail ? 1 : 0);
		m_u32SelSize = 0;
		m_fLMDown = true;
		m_Wnd.SetFocus();
		m_Wnd.SetCapture();
		m_Wnd.RedrawWindow();

		return 0;
	}

	auto CDWFontChooseSampleText::WMLButtonUp([[maybe_unused]] const MSG& msg)->LRESULT
	{
		m_fLMDown = false;
		::ReleaseCapture();

		return TRUE;
	}

	auto CDWFontChooseSampleText::WMMouseMove(const MSG& msg)->LRESULT
	{
		if (!m_fLMDown || !m_pLayoutData) {
			return 0;
		}

		const POINT pt { .x { ut::GetXLPARAM(msg.lParam) }, .y { ut::GetYLPARAM(msg.lParam) } };
		BOOL fIsTrail;
		BOOL fIsInside;
		DWRITE_HIT_TEST_METRICS htm;
		m_pLayoutData->HitTestPoint(DIPFromPixels(pt.x), DIPFromPixels(pt.y) + DIPFromPixels(m_Wnd.GetScrollPos(true)),
			&fIsTrail, &fIsInside, &htm);
		if (fIsInside) {
			if (htm.textPosition == m_u32SelClick) {
				m_u32SelStart = m_u32SelClick;
				m_u32SelSize = fIsTrail ? 1 : 0;
			}
			else if (htm.textPosition > m_u32SelClick) {
				m_u32SelStart = m_u32SelClick;
				m_u32SelSize = (htm.textPosition - m_u32SelClick) + (fIsTrail ? 1 : 0);
				m_u32CaretPos = m_u32SelStart + m_u32SelSize;
			}
			else { //htm.textPosition < m_u32SelClick.
				m_u32SelStart = htm.textPosition + (fIsTrail ? 1 : 0);
				m_u32SelSize = m_u32SelClick - m_u32SelStart;
				m_u32CaretPos = m_u32SelStart;
			}

			m_Wnd.RedrawWindow();
		}

		return 0;
	}

	auto CDWFontChooseSampleText::WMMouseWheel(const MSG& msg)->LRESULT
	{
		const auto iDelta = GET_WHEEL_DELTA_WPARAM(msg.wParam);

		if (::GetAsyncKeyState(VK_CONTROL) < 0) {
			const UINT32 u32CtrlID = m_Wnd.GetDlgCtrlID();
			NMHDR hdr { .hwndFrom { m_Wnd }, .idFrom { u32CtrlID }, .code {
				iDelta > 0 ? WM_MOSEWHEELUPCTRL : WM_MOSEWHEELDOWNCTRL } };
			m_Wnd.GetParent().SendMsg(WM_NOTIFY, u32CtrlID, reinterpret_cast<LPARAM>(&hdr));
		}
		else {
			const auto iScrollY = (m_Wnd.GetClientRect().Height() / 4) * (iDelta > 0 ? -1 : 1);
			m_Wnd.SetScrollPos(true, m_Wnd.GetScrollPos(true) + iScrollY);
			m_Wnd.RedrawWindow();
		}

		return 0;
	}

	auto CDWFontChooseSampleText::WMPaint()->LRESULT
	{
		::ValidateRect(m_Wnd, nullptr);
		m_pD2DDeviceContext->BeginDraw();
		m_pD2DDeviceContext->Clear(D2D1::ColorF(D2D1::ColorF::White));
		if (!m_pLayoutData) {
			m_pD2DDeviceContext->EndDraw();
			m_pDXGISwapChain->Present(0, 0);
			return 0;
		}

		m_pLayoutData->SetDrawingEffect(nullptr, { .startPosition { 0 }, .length { GetDataSize() } });
		m_pLayoutData->SetUnderline(m_fIsUnderline, { .startPosition { 0 }, .length { GetDataSize() } });
		m_pLayoutData->SetStrikethrough(m_fIsStrikethrough, { .startPosition { 0 }, .length { GetDataSize() } });
		if (m_u32SelSize > 0) {
			m_pLayoutData->SetDrawingEffect(&m_effSelection, { .startPosition { m_u32SelStart },
				.length { m_u32SelSize } });
		}

		m_pLayoutData->Draw(nullptr, &m_D2DTextRenderer, 0, -DIPFromPixels(m_Wnd.GetScrollPos(true)));

		if (m_u32CaretPos != 0xFFFFFFFFU) { //Draw caret.
			float flX;
			float flY;
			DWRITE_HIT_TEST_METRICS htm;
			m_pLayoutData->HitTestTextPosition(m_u32CaretPos, FALSE, &flX, &flY, &htm);
			m_pD2DDeviceContext->DrawLine(
				D2D1::Point2F(htm.left, htm.top - DIPFromPixels(m_Wnd.GetScrollPos(true))),
				D2D1::Point2F(htm.left, (htm.top + htm.height) - DIPFromPixels(m_Wnd.GetScrollPos(true))),
				m_pD2DBrushBlack);
		}

		m_pD2DDeviceContext->EndDraw();
		m_pDXGISwapChain->Present(0, 0);

		return 0;
	}

	auto CDWFontChooseSampleText::WMSetCursor(const MSG& msg)->LRESULT
	{
		if (const auto wHitTest = LOWORD(msg.lParam); wHitTest == HTCLIENT) {
			static const auto hCurBeam = reinterpret_cast<HCURSOR>(::LoadImageW(nullptr, IDC_IBEAM, IMAGE_CURSOR, 0, 0, LR_SHARED));
			::SetCursor(hCurBeam);
			return TRUE;
		}

		return GDIUT::DefWndProc(msg); //Default cursor.
	}

	auto CDWFontChooseSampleText::WMSize(const MSG& msg)->LRESULT
	{
		if (!m_pDXGISwapChain) {
			return 0;
		};

		const auto wWidth = LOWORD(msg.lParam);
		const auto wHeight = HIWORD(msg.lParam);
		m_pD2DDeviceContext->SetTarget(nullptr);
		m_pD2DTargetBitmap.safe_release();
		m_pDXGISwapChain->ResizeBuffers(0, wWidth, wHeight, DXGI_FORMAT_UNKNOWN, 0);
		m_pD2DTargetBitmap = DXUT::D2DCreateBitmapFromDxgiSurface(m_pD2DDeviceContext, m_pDXGISwapChain);
		m_pD2DDeviceContext->SetTarget(m_pD2DTargetBitmap);
		RecalcScroll();

		return 0;
	}

	auto CDWFontChooseSampleText::WMVScroll(const MSG& msg)->LRESULT
	{
		auto si = m_Wnd.GetScrollInfo(true);
		switch (LOWORD(msg.wParam)) {
		case SB_TOP:
			si.nPos = si.nMin;
			break;
		case SB_BOTTOM:
			si.nPos = si.nMax;
			break;
		case SB_LINEUP:
			si.nPos -= PixelsFromDIP(5);
			break;
		case SB_LINEDOWN:
			si.nPos += PixelsFromDIP(5);
			break;
		case SB_PAGEUP:
			si.nPos -= si.nPage;
			break;
		case SB_PAGEDOWN:
			si.nPos += si.nPage;
			break;
		case SB_THUMBTRACK:
			si.nPos = si.nTrackPos;
			break;
		default:
			break;
		}

		si.fMask = SIF_POS;
		m_Wnd.SetScrollInfo(true, si);
		m_Wnd.RedrawWindow();

		return 0;
	}

	auto CDWFontChooseSampleText::WMDPIChangedAfterParent()->LRESULT
	{
		const auto flDPI = static_cast<float>(::GetDpiForWindow(m_Wnd));
		m_flDPIScale = flDPI / USER_DEFAULT_SCREEN_DPI;
		m_pD2DDeviceContext->SetDpi(flDPI, flDPI);
		RecalcScroll();

		return 0;
	}

	auto CDWFontChooseSampleText::SubclassProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
		UINT_PTR uIDSubclass, [[maybe_unused]] DWORD_PTR dwRefData) -> LRESULT
	{
		if (uMsg == WM_NCDESTROY) {
			::RemoveWindowSubclass(hWnd, SubclassProc, uIDSubclass);
		}

		const auto pCtrl = reinterpret_cast<CDWFontChooseSampleText*>(uIDSubclass);
		return pCtrl->ProcessMsg({ .hwnd { hWnd }, .message { uMsg }, .wParam { wParam }, .lParam { lParam } });
	}


	//class CDWFontChooseDlg.

	class CDWFontChooseDlg final {
	public:
		auto DoModal(const DWFONTCHOOSEINFO& fci) -> INT_PTR;
		[[nodiscard]] auto GetData() -> DWFONTINFO&;
		[[nodiscard]] auto ProcessMsg(const MSG& msg) -> INT_PTR;
	private:
		void EditSizeIncDec(int iSizeToAdd);
		[[nodiscard]] auto GetComboFamilyRowIDFromData(EDWFontFamily eFF)const -> int;
		[[nodiscard]] auto GetComboFamilySelection()const -> EDWFontFamily;
		[[nodiscard]] auto GetEditFontSize()const -> float;
		[[nodiscard]] auto GetFontInfo() -> DWFONTINFO;
		void FontFaceChoosen();
		void OnCancel();
		void OnCommandComboFontFamilies();
		void OnCheckStrikethrough();
		void OnCheckUnderline();
		void OnCommandCombo(DWORD dwCtrlID, DWORD dwCode);
		void OnCommandEdit(DWORD dwCtrlID, DWORD dwCode);
		void OnOK();
		void SetComboWeightSel(DWORD_PTR dwWeight);
		void SetComboStretchSel(DWORD_PTR dwStretch);
		void SetComboStyleSel(DWORD_PTR dwStyle);
		void SetEditFontSize(float flSize);
		void ShowProperties(bool fShow);
		void SetStatTextFontFacesTotal(std::size_t uzCount);
		void UpdateDynamicLayoutRatios();
		void UpdateFontFamiliesList();
		void UpdateFontFacesList();
		void UpdateSampleText();
		auto WMCommand(const MSG& msg) -> INT_PTR;
		auto WMCtlClrStatic(const MSG& msg) -> INT_PTR;
		auto WMDestroy() -> INT_PTR;
		auto WMInitDialog(const MSG& msg) -> INT_PTR;
		auto WMLButtonDown(const MSG& msg) -> INT_PTR;
		auto WMLButtonUp(const MSG& msg) -> INT_PTR;
		auto WMMouseMove(const MSG& msg) -> INT_PTR;
		auto WMNotify(const MSG& msg) -> INT_PTR;
		auto WMSetCursor() -> INT_PTR;
		auto WMSize(const MSG& msg) -> INT_PTR;
		auto WMDPIChanged(const MSG& msg) -> INT_PTR;
		auto WMGetDPIScaledSize(const MSG& msg) -> INT_PTR;
	private:
		static constexpr int m_arrIDsFaceProps[] = { IDC_STATIC_GRP_FACE_PROPS,
			IDC_STATIC_FAM_NAME, IDC_STATIC_FAM_NAME_DATA, IDC_STATIC_TYPO_FAM_NAME,
			IDC_STATIC_TYPO_FAM_NAME_DATA, IDC_STATIC_W_S_S_FACE_NAME, IDC_STATIC_W_S_S_FACE_NAME_DATA,
			IDC_STATIC_FULL_NAME, IDC_STATIC_FULL_NAME_DATA, IDC_STATIC_WIN32_FAM_NAME,
			IDC_STATIC_WIN32_FAM_NAME_DATA, IDC_STATIC_PS_NAME, IDC_STATIC_PS_NAME_DATA,
			IDC_STATIC_D_S_L_TAG, IDC_STATIC_D_S_L_TAG_DATA, IDC_STATIC_S_S_L_TAG,
			IDC_STATIC_S_S_L_TAG_DATA, IDC_STATIC_SEM_TAG, IDC_STATIC_SEM_TAG_DATA,
			IDC_STATIC_WEIGHTPROP, IDC_STATIC_WEIGHTPROP_DATA, IDC_STATIC_STRETCHPROP,
			IDC_STATIC_STRETCHPROP_DATA, IDC_STATIC_STYLEPROP, IDC_STATIC_STYLEPROP_DATA,
			IDC_STATIC_TYPO_FACE_NAME, IDC_STATIC_TYPO_FACE_NAME_DATA };
		CDWFontChooseList m_FontFamilies;
		CDWFontChooseList m_FontFaces;
		CDWFontChooseSampleText m_SampleText;
		DWFONTINFO m_fi; //Struct to return, after the dialog closes.
		DWFONTCHOOSEINFO m_fci;
		std::vector<DXUT::DWFONTFAMILYINFO> m_vecFFI; //All system fonts.
		std::vector<FONTDATA> m_vecFamilies; //Currently displayed font families strings.
		std::vector<FONTDATA> m_vecFaces;    //Currently displayed font faces strings.
		GDIUT::CSplitter m_SplitHorz;
		GDIUT::CSplitter m_SplitVert;
		GDIUT::CDynLayout m_DynLayout;
		GDIUT::CWnd m_Wnd;
		GDIUT::CWnd m_WndFontFamily;
		GDIUT::CWnd m_WndFontFace;
		GDIUT::CWnd m_WndFontSample;
		GDIUT::CWnd m_WndStatTotalFamilies;
		GDIUT::CWnd m_WndStatSize;
		GDIUT::CWndEdit m_EditSize;
		GDIUT::CWndCombo m_ComboFamily;
		GDIUT::CWndCombo m_ComboWeight;
		GDIUT::CWndCombo m_ComboStretch;
		GDIUT::CWndCombo m_ComboStyle;
		GDIUT::CPoint m_ptFontSizeClick;
		bool m_fCurResize { };
		bool m_fLMDownSize { };
		bool m_fPropertiesShown { };
	};

	auto CDWFontChooseDlg::DoModal(const DWFONTCHOOSEINFO& fci)->INT_PTR
	{
		m_fci = fci;
		return ::DialogBoxParamW(fci.hInstRes, MAKEINTRESOURCEW(IDD_DWFONTCHOOSE),
			fci.hWndParent, GDIUT::DlgProc<CDWFontChooseDlg>, reinterpret_cast<LPARAM>(this));
	}

	auto CDWFontChooseDlg::GetData()->DWFONTINFO& {
		return m_fi;
	}

	auto CDWFontChooseDlg::ProcessMsg(const MSG& msg)->INT_PTR
	{
		switch (msg.message) {
		case WM_COMMAND: return WMCommand(msg);
		case WM_CTLCOLORSTATIC: return WMCtlClrStatic(msg);
		case WM_DESTROY: return WMDestroy();
		case WM_DPICHANGED: return WMDPIChanged(msg);
		case WM_GETDPISCALEDSIZE: return WMGetDPIScaledSize(msg);
		case WM_INITDIALOG: return WMInitDialog(msg);
		case WM_LBUTTONDOWN: return WMLButtonDown(msg);
		case WM_LBUTTONUP: return WMLButtonUp(msg);
		case WM_MOUSEMOVE: return WMMouseMove(msg);
		case WM_NOTIFY: return WMNotify(msg);
		case WM_SETCURSOR: return WMSetCursor();
		case WM_SIZE: return WMSize(msg);
		default: return 0;
		}
	}


	//Private methods.

	void CDWFontChooseDlg::EditSizeIncDec(int iSizeToAdd) {
		SetEditFontSize(GetEditFontSize() + iSizeToAdd);
	}

	auto CDWFontChooseDlg::GetComboFamilyRowIDFromData(EDWFontFamily eFF)const->int
	{
		for (int i = 0; i < m_ComboFamily.GetCount(); ++i) {
			if (m_ComboFamily.GetItemData(i) == static_cast<DWORD_PTR>(eFF)) {
				return i;
			}
		}

		return -1;
	}

	auto CDWFontChooseDlg::GetComboFamilySelection()const->EDWFontFamily
	{
		return static_cast<EDWFontFamily>(m_ComboFamily.GetItemData(m_ComboFamily.GetCurSel()));
	}

	void CDWFontChooseDlg::FontFaceChoosen()
	{
		const auto idx = m_FontFaces.GetSelectedIndex();
		if (idx >= m_vecFaces.size()) {
			return;
		}


		const auto& fd = m_vecFaces[idx];
		const auto pFamilyInfo = fd.pFamilyInfo;
		const auto pFaceInfo = fd.pFaceInfo;
		SetComboWeightSel(std::stoi(pFaceInfo->wstrWeight));
		SetComboStretchSel(std::stoi(pFaceInfo->wstrStretch));
		SetComboStyleSel(std::stoi(pFaceInfo->wstrStyle));

		m_Wnd.GetDlgItem(IDC_STATIC_FAM_NAME_DATA).SetWndText(pFamilyInfo->wstrFamilyName);
		m_Wnd.GetDlgItem(IDC_STATIC_TYPO_FAM_NAME_DATA).SetWndText(pFaceInfo->wstrTypographicFamilyName);
		m_Wnd.GetDlgItem(IDC_STATIC_W_S_S_FACE_NAME_DATA).SetWndText(pFaceInfo->wstrWeightStretchStyleFaceName);
		m_Wnd.GetDlgItem(IDC_STATIC_FULL_NAME_DATA).SetWndText(pFaceInfo->wstrFullName);
		m_Wnd.GetDlgItem(IDC_STATIC_WIN32_FAM_NAME_DATA).SetWndText(pFaceInfo->wstrWin32FamilyName);
		m_Wnd.GetDlgItem(IDC_STATIC_PS_NAME_DATA).SetWndText(pFaceInfo->wstrPostScriptName);

		std::wstring wstrDSLTag = L"[";
		auto iIndex = 0;
		for (const auto& wstr : pFaceInfo->vecDesignScriptLangTag) {
			wstrDSLTag += wstr;
			if (iIndex++ < pFaceInfo->vecDesignScriptLangTag.size() - 1) {
				wstrDSLTag += L", ";
			}
		}
		wstrDSLTag += L"]";
		m_Wnd.GetDlgItem(IDC_STATIC_D_S_L_TAG_DATA).SetWndText(wstrDSLTag);

		std::wstring wstrSSLTag = L"[";
		iIndex = 0;
		for (const auto& wstr : pFaceInfo->vecSuppScriptLangTag) {
			wstrSSLTag += wstr;
			if (iIndex++ < pFaceInfo->vecSuppScriptLangTag.size() - 1) {
				wstrSSLTag += L", ";
			}
		}
		wstrSSLTag += L"]";
		m_Wnd.GetDlgItem(IDC_STATIC_S_S_L_TAG_DATA).SetWndText(wstrSSLTag);

		std::wstring wstrSemTag = L"[";
		iIndex = 0;
		for (const auto& wstr : pFaceInfo->vecSemanticTag) {
			wstrSemTag += wstr;
			if (iIndex++ < pFaceInfo->vecSemanticTag.size() - 1) {
				wstrSemTag += L", ";
			}
		}
		wstrSemTag += L"]";

		m_Wnd.GetDlgItem(IDC_STATIC_SEM_TAG_DATA).SetWndText(wstrSemTag);
		m_Wnd.GetDlgItem(IDC_STATIC_WEIGHTPROP_DATA).SetWndText(pFaceInfo->wstrWeight);
		m_Wnd.GetDlgItem(IDC_STATIC_STRETCHPROP_DATA).SetWndText(pFaceInfo->wstrStretch);
		m_Wnd.GetDlgItem(IDC_STATIC_STYLEPROP_DATA).SetWndText(pFaceInfo->wstrStyle);
		m_Wnd.GetDlgItem(IDC_STATIC_TYPO_FACE_NAME_DATA).SetWndText(pFaceInfo->wstrTypographicFaceName);
		UpdateSampleText();
	}

	void CDWFontChooseDlg::OnCheckStrikethrough()
	{
		m_SampleText.SetStrikethrough(GDIUT::CWndBtn(m_Wnd.GetDlgItem(IDC_CHK_STRIKETHROUGH)).IsChecked());
	}

	void CDWFontChooseDlg::OnCheckUnderline()
	{
		m_SampleText.SetUnderline(GDIUT::CWndBtn(m_Wnd.GetDlgItem(IDC_CHK_UNDERLINE)).IsChecked());
	}

	auto CDWFontChooseDlg::GetEditFontSize()const->float {
		return std::clamp(m_EditSize.IsWndTextEmpty() ? 1.F : std::stof(m_EditSize.GetWndText()), 1.F, 1296.F);
	}

	auto CDWFontChooseDlg::GetFontInfo()->DWFONTINFO
	{
		const auto idx = m_FontFamilies.GetSelectedIndex();
		if (idx >= m_vecFamilies.size()) {
			return { };
		}

		const auto& wstrFamily = m_vecFamilies[idx].pFamilyInfo->wstrFamilyName;
		const auto eWeight = static_cast<DWRITE_FONT_WEIGHT>(m_ComboWeight.GetItemData(m_ComboWeight.GetCurSel()));
		const auto eStretch = static_cast<DWRITE_FONT_STRETCH>(m_ComboStretch.GetItemData(m_ComboStretch.GetCurSel()));
		const auto eStyle = static_cast<DWRITE_FONT_STYLE>(m_ComboStyle.GetItemData(m_ComboStyle.GetCurSel()));
		const auto flSizeDIP = ut::FontPixelsFromPoints(GetEditFontSize());
		return { .wstrFamilyName { wstrFamily }, .wstrLocale { m_fci.wstrLocale }, .eWeight { eWeight },
			.eStretch { eStretch }, .eStyle { eStyle }, .flSizeDIP { flSizeDIP } };
	}

	void CDWFontChooseDlg::OnCommandCombo(DWORD dwCtrlID, DWORD dwCode)
	{
		if (dwCode == CBN_SELCHANGE) {
			if (dwCtrlID == IDC_COMBO_FONT_FAMILY) {
				OnCommandComboFontFamilies();
			}
			else {
				UpdateSampleText();
			}
		}
	}

	void CDWFontChooseDlg::OnCommandComboFontFamilies()
	{
		UpdateFontFamiliesList();
	}

	void CDWFontChooseDlg::OnCommandEdit([[maybe_unused]] DWORD dwCtrlID, DWORD dwCode)
	{
		if (dwCode == EN_CHANGE) {
			UpdateSampleText();
		}
	}

	void CDWFontChooseDlg::OnOK()
	{
		m_fi = GetFontInfo();
		m_Wnd.EndDialog(IDOK);
	}

	void CDWFontChooseDlg::SetComboWeightSel(DWORD_PTR dwWeight)
	{
		for (auto i = 0; i < m_ComboWeight.GetCount(); ++i) {
			if (m_ComboWeight.GetItemData(i) == dwWeight) {
				m_ComboWeight.SetCurSel(i);
			}
		}
	}

	void CDWFontChooseDlg::SetComboStretchSel(DWORD_PTR dwStretch)
	{
		for (auto i = 0; i < m_ComboStretch.GetCount(); ++i) {
			if (m_ComboStretch.GetItemData(i) == dwStretch) {
				m_ComboStretch.SetCurSel(i);
			}
		}
	}

	void CDWFontChooseDlg::SetComboStyleSel(DWORD_PTR dwStyle)
	{
		for (auto i = 0; i < m_ComboStyle.GetCount(); ++i) {
			if (m_ComboStyle.GetItemData(i) == dwStyle) {
				m_ComboStyle.SetCurSel(i);
			}
		}
	}

	void CDWFontChooseDlg::SetEditFontSize(float flSize) {
		m_EditSize.SetWndText(std::format(L"{}pt", std::clamp(flSize, 1.F, 1296.F)));
	}

	void CDWFontChooseDlg::ShowProperties(bool fShow)
	{
		m_DynLayout.Enable(false);
		auto hdwp = ::BeginDeferWindowPos(static_cast<int>(std::size(m_arrIDsFaceProps)));
		for (const auto id : m_arrIDsFaceProps) {
			const auto hWnd = m_Wnd.GetDlgItem(id);
			hdwp = ::DeferWindowPos(hdwp, hWnd, nullptr, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE | SWP_NOACTIVATE
				| (fShow ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
		}
		::EndDeferWindowPos(hdwp);
		const auto rcWnd = m_Wnd.GetWindowRect();
		auto rcSampleText = m_Wnd.GetDlgItem(IDC_CUSTOM_FONT_SAMPLE).GetWindowRect();
		const auto iHeight = rcWnd.Height();
		int iWidth;

		if (fShow) {
			const auto rcGrpProps = m_Wnd.GetDlgItem(IDC_STATIC_GRP_FACE_PROPS).GetWindowRect();
			const auto iIndent = rcGrpProps.left - rcSampleText.right;
			const auto iRight = rcSampleText.Width() + rcGrpProps.Width() + (iIndent * 2);
			GDIUT::CRect rcWndNew(0, 0, iRight, 0);
			::AdjustWindowRect(rcWndNew, m_Wnd.GetWindowStyles(), FALSE);
			iWidth = rcWndNew.Width();
		}
		else {
			::AdjustWindowRect(rcSampleText, m_Wnd.GetWindowStyles(), FALSE);
			iWidth = rcSampleText.Width();
		}
		m_Wnd.SetWindowPos(nullptr, 0, 0, iWidth, iHeight, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
		m_Wnd.GetDlgItem(IDC_BTN_PROPERTIES).SetWndText(fShow ? L"Properties <<" : L"Properties >>");
		m_fPropertiesShown = fShow;
		m_DynLayout.Enable(true);
	}

	void CDWFontChooseDlg::SetStatTextFontFacesTotal(std::size_t uzCount)
	{
		GDIUT::CWnd(m_Wnd.GetDlgItem(IDC_STATIC_TOTALFACES)).SetWndText(std::format(L"Total Faces: {}", uzCount));
	}

	void CDWFontChooseDlg::UpdateDynamicLayoutRatios()
	{
		using GDIUT::CDynLayout;
		const auto rcFamily = m_WndFontFamily.GetClientRect();
		const auto rcSample = m_WndFontSample.GetClientRect();
		const auto iFamilySizeH = std::lround(static_cast<float>(rcFamily.Width()) / rcSample.Width() * 100);
		m_DynLayout.UpdateItem(m_WndFontFamily, CDynLayout::MoveNone(), CDynLayout::SizeHorzAndVert(iFamilySizeH, 50));
		m_DynLayout.UpdateItem(m_WndStatTotalFamilies, CDynLayout::MoveHorzAndVert(iFamilySizeH, 50), CDynLayout::SizeNone());
		m_DynLayout.UpdateItem(m_WndFontFace, CDynLayout::MoveHorz(iFamilySizeH), CDynLayout::SizeHorzAndVert(100 - iFamilySizeH, 50));
	}

	void CDWFontChooseDlg::UpdateFontFamiliesList()
	{
		const auto lmbFFIToFD = [](const DXUT::DWFONTFAMILYINFO& FFamilyInfo) {
			const auto& FFaceInfo = FFamilyInfo.vecFontFaceInfo.at(0);
			return FONTDATA { .wstrString { FFamilyInfo.wstrFamilyName },
				.pFamilyInfo { &FFamilyInfo }, .pFaceInfo { &FFaceInfo } }; };
		m_vecFamilies.clear();

		using enum EDWFontFamily;
		switch (GetComboFamilySelection()) {
		case FAMILY_ALL:
			std::ranges::copy(m_vecFFI | std::views::transform(lmbFFIToFD), std::back_inserter(m_vecFamilies));
			break;
		case FAMILY_MONOSPACED:
		{
			auto filter = m_vecFFI | std::views::filter([](const DXUT::DWFONTFAMILYINFO& ffi) {
				return ffi.fIsMonospaced; });
			std::ranges::copy(filter | std::views::transform(lmbFFIToFD), std::back_inserter(m_vecFamilies));
		}
		break;
		case FAMILY_NONMONOSPACED:
		{
			auto filter = m_vecFFI | std::views::filter([](const DXUT::DWFONTFAMILYINFO& ffi) {
				return !ffi.fIsMonospaced; });
			std::ranges::copy(filter | std::views::transform(lmbFFIToFD), std::back_inserter(m_vecFamilies));
		}
		break;
		default:
			break;
		}

		m_FontFamilies.SetData(m_vecFamilies);

		if (m_vecFamilies.empty()) {
			m_vecFaces.clear();
			m_FontFaces.SetData(m_vecFaces); //To empty the data in the m_FontFaces list.
			SetStatTextFontFacesTotal(0);
		}

		m_WndStatTotalFamilies.SetWndText(std::format(L"Total: {}", m_vecFamilies.size()));
	}

	void CDWFontChooseDlg::UpdateFontFacesList()
	{
		m_vecFaces.clear();
		const auto idx = m_FontFamilies.GetSelectedIndex();
		if (idx >= m_vecFamilies.size()) {
			m_FontFaces.SetData(m_vecFaces);
			SetStatTextFontFacesTotal(0);
			return;
		}

		const auto pFamilyInfo = m_vecFamilies.at(idx).pFamilyInfo;
		const auto lmbFFIToFD = [=](const DXUT::DWFONTFACEINFO& FFaceInfo) {
			return FONTDATA { .wstrString { FFaceInfo.wstrWeightStretchStyleFaceName },
				.pFamilyInfo { pFamilyInfo }, .pFaceInfo { &FFaceInfo } }; };
		std::ranges::copy(pFamilyInfo->vecFontFaceInfo | std::views::transform(lmbFFIToFD), std::back_inserter(m_vecFaces));
		m_FontFaces.SetData(m_vecFaces);
		SetStatTextFontFacesTotal(m_vecFaces.size());
	}

	void CDWFontChooseDlg::UpdateSampleText()
	{
		if (m_vecFFI.empty()) {
			return;
		}

		m_SampleText.SetFontInfo(GetFontInfo());
	}

	auto CDWFontChooseDlg::WMCommand(const MSG& msg)->INT_PTR
	{
		const auto uCtrlID = LOWORD(msg.wParam);
		const auto uCode = HIWORD(msg.wParam); //Control code, zero for menu.

		switch (uCtrlID) {
		case IDOK: break; //Empty handler, to prevent dialog closing on Enter.
		case IDC_BTN_OK:
			OnOK();
			break;
		case IDCANCEL:
			m_Wnd.EndDialog(IDCANCEL);
			break;
		case IDC_BTN_PROPERTIES:
			ShowProperties(!m_fPropertiesShown);
			break;
		case IDC_COMBO_FONT_FAMILY:
		case IDC_COMBO_FONT_WEIGHT:
		case IDC_COMBO_FONT_STRETCH:
		case IDC_COMBO_FONT_STYLE:
			OnCommandCombo(uCtrlID, uCode);
			break;
		case IDC_EDIT_FONT_SIZE:
			OnCommandEdit(uCtrlID, uCode);
			break;
		case IDC_CHK_UNDERLINE:
			OnCheckUnderline();
			break;
		case IDC_CHK_STRIKETHROUGH:
			OnCheckStrikethrough();
			break;
		default:
			return FALSE;
		}

		return TRUE;
	}

	auto CDWFontChooseDlg::WMCtlClrStatic(const MSG& msg) -> INT_PTR
	{
		static constexpr int arrIDsData[] = { IDC_STATIC_FAM_NAME_DATA, IDC_STATIC_TYPO_FAM_NAME_DATA,
			IDC_STATIC_W_S_S_FACE_NAME_DATA, IDC_STATIC_FULL_NAME_DATA, IDC_STATIC_WIN32_FAM_NAME_DATA,
			IDC_STATIC_PS_NAME_DATA, IDC_STATIC_D_S_L_TAG_DATA, IDC_STATIC_S_S_L_TAG_DATA, IDC_STATIC_SEM_TAG_DATA,
			IDC_STATIC_WEIGHTPROP_DATA, IDC_STATIC_STRETCHPROP_DATA, IDC_STATIC_STYLEPROP_DATA,
			IDC_STATIC_TYPO_FACE_NAME_DATA };

		if (const auto hWndFrom = reinterpret_cast<HWND>(msg.lParam);
			std::ranges::any_of(arrIDsData, [=](int id) { return m_Wnd.GetDlgItem(id) == hWndFrom; })) {
			const auto hDC = reinterpret_cast<HDC>(msg.wParam);
			::SetTextColor(hDC, RGB(0, 50, 250));
			::SetBkColor(hDC, ::GetSysColor(COLOR_3DFACE));
			return reinterpret_cast<INT_PTR>(::GetSysColorBrush(COLOR_3DFACE));
		}

		return FALSE;
	}

	auto CDWFontChooseDlg::WMDestroy()->INT_PTR
	{
		m_vecFFI.clear();
		return TRUE;
	};

	auto CDWFontChooseDlg::WMDPIChanged([[maybe_unused]] const MSG & msg)->INT_PTR
	{
		m_DynLayout.Enable(true);
		return 0;
	}

	auto CDWFontChooseDlg::WMGetDPIScaledSize([[maybe_unused]] const MSG & msg)->INT_PTR
	{
		m_DynLayout.Enable(false);
		return 0;
	}

	auto CDWFontChooseDlg::WMInitDialog(const MSG& msg)->INT_PTR
	{
		m_Wnd.Attach(msg.hwnd);
		m_WndFontFamily.Attach(m_Wnd.GetDlgItem(IDC_CUSTOM_FONT_FAMILY));
		m_WndFontFace.Attach(m_Wnd.GetDlgItem(IDC_CUSTOM_FONT_FACE));
		m_WndFontSample.Attach(m_Wnd.GetDlgItem(IDC_CUSTOM_FONT_SAMPLE));
		m_WndStatTotalFamilies.Attach(m_Wnd.GetDlgItem(IDC_STATIC_TOTALFAMILIES));
		m_WndStatSize.Attach(m_Wnd.GetDlgItem(IDC_STATIC_SIZE));
		m_EditSize.Attach(m_Wnd.GetDlgItem(IDC_EDIT_FONT_SIZE));
		m_ComboFamily.Attach(m_Wnd.GetDlgItem(IDC_COMBO_FONT_FAMILY));
		m_ComboWeight.Attach(m_Wnd.GetDlgItem(IDC_COMBO_FONT_WEIGHT));
		m_ComboStretch.Attach(m_Wnd.GetDlgItem(IDC_COMBO_FONT_STRETCH));
		m_ComboStyle.Attach(m_Wnd.GetDlgItem(IDC_COMBO_FONT_STYLE));
		SetEditFontSize(35);

		using enum EDWFontFamily;
		auto iIndex = m_ComboFamily.AddString(L"All");
		m_ComboFamily.SetItemData(iIndex, static_cast<DWORD_PTR>(FAMILY_ALL));
		iIndex = m_ComboFamily.AddString(L"Monospaced");
		m_ComboFamily.SetItemData(iIndex, static_cast<DWORD_PTR>(FAMILY_MONOSPACED));
		iIndex = m_ComboFamily.AddString(L"Non-Monospaced");
		m_ComboFamily.SetItemData(iIndex, static_cast<DWORD_PTR>(FAMILY_NONMONOSPACED));
		m_ComboFamily.SetCurSel(GetComboFamilyRowIDFromData(m_fci.eFontFamily));

		iIndex = m_ComboWeight.AddString(L"Thin");
		m_ComboWeight.SetItemData(iIndex, DWRITE_FONT_WEIGHT_THIN);
		iIndex = m_ComboWeight.AddString(L"Extra-light");
		m_ComboWeight.SetItemData(iIndex, DWRITE_FONT_WEIGHT_EXTRA_LIGHT);
		iIndex = m_ComboWeight.AddString(L"Light");
		m_ComboWeight.SetItemData(iIndex, DWRITE_FONT_WEIGHT_LIGHT);
		iIndex = m_ComboWeight.AddString(L"Semi-light");
		m_ComboWeight.SetItemData(iIndex, DWRITE_FONT_WEIGHT_SEMI_LIGHT);
		iIndex = m_ComboWeight.AddString(L"Normal");
		m_ComboWeight.SetItemData(iIndex, DWRITE_FONT_WEIGHT_NORMAL);
		iIndex = m_ComboWeight.AddString(L"Medium");
		m_ComboWeight.SetItemData(iIndex, DWRITE_FONT_WEIGHT_MEDIUM);
		iIndex = m_ComboWeight.AddString(L"Semi-bold");
		m_ComboWeight.SetItemData(iIndex, DWRITE_FONT_WEIGHT_SEMI_BOLD);
		iIndex = m_ComboWeight.AddString(L"Bold");
		m_ComboWeight.SetItemData(iIndex, DWRITE_FONT_WEIGHT_BOLD);
		iIndex = m_ComboWeight.AddString(L"Extra-bold");
		m_ComboWeight.SetItemData(iIndex, DWRITE_FONT_WEIGHT_EXTRA_BOLD);
		iIndex = m_ComboWeight.AddString(L"Black");
		m_ComboWeight.SetItemData(iIndex, DWRITE_FONT_WEIGHT_BLACK);
		iIndex = m_ComboWeight.AddString(L"Extra-black");
		m_ComboWeight.SetItemData(iIndex, DWRITE_FONT_WEIGHT_EXTRA_BLACK);

		iIndex = m_ComboStretch.AddString(L"Ultra-condensed");
		m_ComboStretch.SetItemData(iIndex, DWRITE_FONT_STRETCH_ULTRA_CONDENSED);
		iIndex = m_ComboStretch.AddString(L"Extra-condensed");
		m_ComboStretch.SetItemData(iIndex, DWRITE_FONT_STRETCH_EXTRA_CONDENSED);
		iIndex = m_ComboStretch.AddString(L"Condensed");
		m_ComboStretch.SetItemData(iIndex, DWRITE_FONT_STRETCH_CONDENSED);
		iIndex = m_ComboStretch.AddString(L"Semi-condensed");
		m_ComboStretch.SetItemData(iIndex, DWRITE_FONT_STRETCH_SEMI_CONDENSED);
		iIndex = m_ComboStretch.AddString(L"Normal");
		m_ComboStretch.SetItemData(iIndex, DWRITE_FONT_STRETCH_NORMAL);
		iIndex = m_ComboStretch.AddString(L"Semi-expanded");
		m_ComboStretch.SetItemData(iIndex, DWRITE_FONT_STRETCH_SEMI_EXPANDED);
		iIndex = m_ComboStretch.AddString(L"Expanded");
		m_ComboStretch.SetItemData(iIndex, DWRITE_FONT_STRETCH_EXPANDED);
		iIndex = m_ComboStretch.AddString(L"Extra-expanded");
		m_ComboStretch.SetItemData(iIndex, DWRITE_FONT_STRETCH_EXTRA_EXPANDED);
		iIndex = m_ComboStretch.AddString(L"Ultra-expanded");
		m_ComboStretch.SetItemData(iIndex, DWRITE_FONT_STRETCH_ULTRA_EXPANDED);

		iIndex = m_ComboStyle.AddString(L"Normal");
		m_ComboStyle.SetItemData(iIndex, DWRITE_FONT_STYLE_NORMAL);
		iIndex = m_ComboStyle.AddString(L"Oblique");
		m_ComboStyle.SetItemData(iIndex, DWRITE_FONT_STYLE_OBLIQUE);
		iIndex = m_ComboStyle.AddString(L"Italic");
		m_ComboStyle.SetItemData(iIndex, DWRITE_FONT_STYLE_ITALIC);

		m_SampleText.Create(m_Wnd, IDC_CUSTOM_FONT_SAMPLE);
		m_vecFFI = DXUT::DWGetSystemFonts(m_fci.wstrLocale.data());
		std::ranges::sort(m_vecFFI, [](const DXUT::DWFONTFAMILYINFO& lhs, const DXUT::DWFONTFAMILYINFO& rhs) {
			return lhs.wstrFamilyName < rhs.wstrFamilyName;	});

		m_FontFamilies.Create(m_Wnd, IDC_CUSTOM_FONT_FAMILY);
		m_FontFaces.Create(m_Wnd, IDC_CUSTOM_FONT_FACE);
		UpdateFontFamiliesList();

		auto rcFSWnd = m_WndFontSample.GetWindowRect();
		m_Wnd.ScreenToClient(rcFSWnd);
		m_SplitHorz.Initialize(m_Wnd, m_WndFontFamily, GDIUT::CSplitter::EAnchorSide::SIDE_RIGHT, 16);
		m_SplitHorz.SetEdges(1, rcFSWnd.right - 1);
		m_SplitHorz.AddItem(m_WndFontFace, true);
		m_SplitHorz.AddItem(m_WndStatTotalFamilies, false);
		m_SplitVert.Initialize(m_Wnd, m_WndFontSample, GDIUT::CSplitter::EAnchorSide::SIDE_TOP, 7);
		m_SplitVert.SetEdges(30, rcFSWnd.bottom - 1);
		m_SplitVert.AddItem(m_WndFontFamily, true);
		m_SplitVert.AddItem(m_WndFontFace, true);
		m_SplitVert.AddItem(IDC_COMBO_FONT_FAMILY, false);
		m_SplitVert.AddItem(m_WndStatTotalFamilies, false);
		m_SplitVert.AddItem(IDC_STATIC_TOTALFACES, false);

		m_DynLayout.SetHost(m_Wnd);
		m_DynLayout.AddItem(m_WndFontFamily, GDIUT::CDynLayout::MoveNone(), GDIUT::CDynLayout::SizeHorzAndVert(50, 50));
		m_DynLayout.AddItem(m_WndFontFace, GDIUT::CDynLayout::MoveHorz(50), GDIUT::CDynLayout::SizeHorzAndVert(50, 50));
		m_DynLayout.AddItem(IDC_COMBO_FONT_FAMILY, GDIUT::CDynLayout::MoveVert(50), GDIUT::CDynLayout::SizeNone());
		m_DynLayout.AddItem(m_WndStatTotalFamilies, GDIUT::CDynLayout::MoveHorzAndVert(50, 50), GDIUT::CDynLayout::SizeNone());
		m_DynLayout.AddItem(IDC_STATIC_TOTALFACES, GDIUT::CDynLayout::MoveHorzAndVert(100, 50), GDIUT::CDynLayout::SizeNone());
		m_DynLayout.AddItem(m_WndFontSample, GDIUT::CDynLayout::MoveVert(50), GDIUT::CDynLayout::SizeHorzAndVert(100, 50));
		m_DynLayout.AddItem(IDC_EDIT_FONT_SIZE, GDIUT::CDynLayout::MoveHorzAndVert(50, 100), GDIUT::CDynLayout::SizeNone());
		m_DynLayout.AddItem(IDC_STATIC_SIZE, GDIUT::CDynLayout::MoveHorzAndVert(50, 100), GDIUT::CDynLayout::SizeNone());
		m_DynLayout.AddItem(IDC_COMBO_FONT_WEIGHT, GDIUT::CDynLayout::MoveHorzAndVert(50, 100), GDIUT::CDynLayout::SizeNone());
		m_DynLayout.AddItem(IDC_STATIC_WEIGHT, GDIUT::CDynLayout::MoveHorzAndVert(50, 100), GDIUT::CDynLayout::SizeNone());
		m_DynLayout.AddItem(IDC_COMBO_FONT_STRETCH, GDIUT::CDynLayout::MoveHorzAndVert(50, 100), GDIUT::CDynLayout::SizeNone());
		m_DynLayout.AddItem(IDC_STATIC_STRETCH, GDIUT::CDynLayout::MoveHorzAndVert(50, 100), GDIUT::CDynLayout::SizeNone());
		m_DynLayout.AddItem(IDC_COMBO_FONT_STYLE, GDIUT::CDynLayout::MoveHorzAndVert(50, 100), GDIUT::CDynLayout::SizeNone());
		m_DynLayout.AddItem(IDC_STATIC_STYLE, GDIUT::CDynLayout::MoveHorzAndVert(50, 100), GDIUT::CDynLayout::SizeNone());
		m_DynLayout.AddItem(IDC_CHK_UNDERLINE, GDIUT::CDynLayout::MoveHorzAndVert(50, 100), GDIUT::CDynLayout::SizeNone());
		m_DynLayout.AddItem(IDC_CHK_STRIKETHROUGH, GDIUT::CDynLayout::MoveHorzAndVert(50, 100), GDIUT::CDynLayout::SizeNone());
		m_DynLayout.AddItem(IDC_BTN_OK, GDIUT::CDynLayout::MoveHorzAndVert(50, 100), GDIUT::CDynLayout::SizeNone());
		m_DynLayout.AddItem(IDCANCEL, GDIUT::CDynLayout::MoveHorzAndVert(50, 100), GDIUT::CDynLayout::SizeNone());
		m_DynLayout.AddItem(IDC_BTN_PROPERTIES, GDIUT::CDynLayout::MoveHorzAndVert(50, 100), GDIUT::CDynLayout::SizeNone());

		for (const auto i : m_arrIDsFaceProps) {
			m_DynLayout.AddItem(i, GDIUT::CDynLayout::MoveHorz(100), GDIUT::CDynLayout::SizeNone());
		}

		m_DynLayout.Enable(true);
		ShowProperties(false);

		return TRUE;
	}

	auto CDWFontChooseDlg::WMLButtonDown(const MSG& msg)->LRESULT
	{
		const POINT pt { .x { ut::GetXLPARAM(msg.lParam) }, .y { ut::GetYLPARAM(msg.lParam) } };
		auto rcStatSize = m_WndStatSize.GetWindowRect();
		m_Wnd.ScreenToClient(rcStatSize);
		if (rcStatSize.PtInRect(pt)) {
			m_ptFontSizeClick = pt;
			m_fLMDownSize = true;
			m_Wnd.SetCapture();
			return TRUE;
		}

		m_SplitHorz.WMLButtonDown(pt.x, pt.y);
		m_SplitVert.WMLButtonDown(pt.x, pt.y);
		if (m_SplitHorz.IsSplitting() || m_SplitVert.IsSplitting()) {
			m_DynLayout.Enable(false);
		}

		return TRUE;
	}

	auto CDWFontChooseDlg::WMLButtonUp([[maybe_unused]] const MSG& msg)->LRESULT
	{
		m_fLMDownSize = false;
		::ReleaseCapture();

		if (m_SplitHorz.IsSplitting()) {
			UpdateDynamicLayoutRatios();
		}

		m_SplitHorz.WMLButtonUp();
		m_SplitVert.WMLButtonUp();
		m_DynLayout.Enable(true);

		return TRUE;
	}

	auto CDWFontChooseDlg::WMMouseMove(const MSG& msg)->LRESULT
	{
		const POINT pt { .x { ut::GetXLPARAM(msg.lParam) }, .y { ut::GetYLPARAM(msg.lParam) } };
		auto rcStatSize = m_WndStatSize.GetWindowRect();
		m_Wnd.ScreenToClient(rcStatSize);
		m_fCurResize = rcStatSize.PtInRect(pt);

		if (m_fLMDownSize) {
			EditSizeIncDec(pt.x - m_ptFontSizeClick.x); //Positive or negative.
			m_ptFontSizeClick.x = pt.x;
		}

		m_SplitHorz.WMMouseMove(pt.x, pt.y);
		m_SplitVert.WMMouseMove(pt.x, pt.y);

		return TRUE;
	}

	auto CDWFontChooseDlg::WMNotify(const MSG& msg)->INT_PTR
	{
		const auto pNMHDR = reinterpret_cast<NMHDR*>(msg.lParam);

		switch (pNMHDR->idFrom) {
		case IDC_CUSTOM_FONT_FAMILY:
			switch (pNMHDR->code) {
			case MSG_ITEM_CHANGED:
				UpdateFontFacesList();
				break;
			default:
				break;
			}
			break;
		case IDC_CUSTOM_FONT_FACE:
			switch (pNMHDR->code) {
			case MSG_ITEM_CHANGED:
				FontFaceChoosen();
				break;
			default:
				break;
			}
			break;
		case IDC_CUSTOM_FONT_SAMPLE:
			EditSizeIncDec(pNMHDR->code == WM_MOSEWHEELUPCTRL ? 1 : (pNMHDR->code == WM_MOSEWHEELDOWNCTRL ? -1 : 0));
			break;
		default:
			break;
		}

		return TRUE;
	}

	auto CDWFontChooseDlg::WMSetCursor()->INT_PTR
	{
		if (m_fCurResize) {
			static const auto hCurResize = static_cast<HCURSOR>(::LoadImageW(nullptr, IDC_SIZEWE, IMAGE_CURSOR, 0, 0, LR_SHARED));
			::SetCursor(hCurResize);
			return TRUE;
		}

		return 0; //Default cursor.
	}

	auto CDWFontChooseDlg::WMSize(const MSG & msg)->LRESULT
	{
		const auto wWidth = LOWORD(msg.lParam);
		const auto wHeight = HIWORD(msg.lParam);
		m_DynLayout.WMSize(wWidth, wHeight);

		auto rcFSWnd = m_WndFontSample.GetWindowRect();
		m_Wnd.ScreenToClient(rcFSWnd);
		m_SplitHorz.SetEdges(1, rcFSWnd.right);
		m_SplitVert.SetEdges(30, rcFSWnd.bottom - 1);
		m_Wnd.RedrawWindow(nullptr, nullptr, RDW_ERASE | RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);

		return TRUE;
	}

	export [[nodiscard]] auto DWFontChoose(const DWFONTCHOOSE::DWFONTCHOOSEINFO & fci) -> std::optional<DWFONTCHOOSE::DWFONTINFO> {
		DWFONTCHOOSE::CDWFontChooseDlg dlg;
		return dlg.DoModal(fci) == IDOK ? std::optional { dlg.GetData() } : std::nullopt;
	}

	export [[nodiscard]] auto DWFontChoose() -> std::optional<DWFONTCHOOSE::DWFONTINFO> {
		return DWFontChoose({ });
	}
};