module;
#include <SDKDDKVer.h>
#include "DWFontChooseRes.h"
#include <Windows.h>
#include <commctrl.h>
#include <d2d1.h>
#include <d2d1_1.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <dwrite_3.h>
#include <algorithm>
#include <cassert>
#include <format>
#include <optional>
#include <source_location>
#include <string>
#include <unordered_map>
#include <vector>
export module DWFontChoose;

import DXUtility;

#pragma comment(linker,"/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#pragma comment(lib, "msimg32")
#pragma comment(lib, "Comctl32")

export struct DWFONTCHOOSEINFO {
	std::wstring wstrLocale { L"en-US" };
	HINSTANCE hInstRes { };
	HWND hWndParent { };
};

namespace DWFONTCHOOSE {
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

		class CDynLayout final {
		public:
			//Ratio settings, for how much to move or resize an item when parent is resized.
			struct ItemRatio { float m_flXRatio { }; float m_flYRatio { }; };
			struct MoveRatio : public ItemRatio { }; //To differentiate move from size in the AddItem.
			struct SizeRatio : public ItemRatio { };

			void AddItem(int iIDItem, MoveRatio move, SizeRatio size);
			void Enable(bool fTrack);
			void OnSize(int iWidth, int iHeight)const; //Should be hooked into the host window WM_SIZE handler.
			void RemoveAll() { m_vecItems.clear(); }
			void SetHost(HWND hWnd) { assert(hWnd != nullptr); m_hWndHost = hWnd; }

			//Static helper methods to use in the AddItem.
			[[nodiscard]] static MoveRatio MoveNone() { return { }; }
			[[nodiscard]] static MoveRatio MoveHorz(int iXRatio)
			{
				iXRatio = std::clamp(iXRatio, 0, 100); return { { .m_flXRatio { iXRatio / 100.F } } };
			}
			[[nodiscard]] static MoveRatio MoveVert(int iYRatio)
			{
				iYRatio = std::clamp(iYRatio, 0, 100); return { { .m_flYRatio { iYRatio / 100.F } } };
			}
			[[nodiscard]] static MoveRatio MoveHorzAndVert(int iXRatio, int iYRatio)
			{
				iXRatio = std::clamp(iXRatio, 0, 100); iYRatio = std::clamp(iYRatio, 0, 100);
				return { { .m_flXRatio { iXRatio / 100.F }, .m_flYRatio { iYRatio / 100.F } } };
			}
			[[nodiscard]] static SizeRatio SizeNone() { return { }; }
			[[nodiscard]] static SizeRatio SizeHorz(int iXRatio)
			{
				iXRatio = std::clamp(iXRatio, 0, 100); return { { .m_flXRatio { iXRatio / 100.F } } };
			}
			[[nodiscard]] static SizeRatio SizeVert(int iYRatio)
			{
				iYRatio = std::clamp(iYRatio, 0, 100); return { { .m_flYRatio { iYRatio / 100.F } } };
			}
			[[nodiscard]] static SizeRatio SizeHorzAndVert(int iXRatio, int iYRatio)
			{
				iXRatio = std::clamp(iXRatio, 0, 100); iYRatio = std::clamp(iYRatio, 0, 100);
				return { { .m_flXRatio { iXRatio / 100.F }, .m_flYRatio { iYRatio / 100.F } } };
			}
		private:
			struct ItemData {
				HWND hWnd { };   //Item window.
				RECT rcOrig { }; //Item original client area rect after EnableTrack(true).
				MoveRatio move;  //How much to move the item.
				SizeRatio size;  //How much to resize the item.
			};
			HWND m_hWndHost { };   //Host window.
			RECT m_rcHostOrig { }; //Host original client area rect after EnableTrack(true).
			std::vector<ItemData> m_vecItems; //All items to resize/move.
			bool m_fTrack { };
		};

		void CDynLayout::AddItem(int iIDItem, MoveRatio move, SizeRatio size)
		{
			const auto hWnd = ::GetDlgItem(m_hWndHost, iIDItem);
			assert(hWnd != nullptr);
			if (hWnd != nullptr) {
				m_vecItems.emplace_back(ItemData { .hWnd { hWnd }, .move { move }, .size { size } });
			}
		}

		void CDynLayout::Enable(bool fTrack)
		{
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

		void CDynLayout::OnSize(int iWidth, int iHeight)const
		{
			if (!m_fTrack)
				return;

			const auto iHostWidth = m_rcHostOrig.right - m_rcHostOrig.left;
			const auto iHostHeight = m_rcHostOrig.bottom - m_rcHostOrig.top;
			const auto iDeltaX = iWidth - iHostWidth;
			const auto iDeltaY = iHeight - iHostHeight;

			const auto hDWP = ::BeginDeferWindowPos(static_cast<int>(m_vecItems.size()));
			for (const auto& [hWnd, rc, move, size] : m_vecItems) {
				const auto iNewLeft = static_cast<int>(rc.left + (iDeltaX * move.m_flXRatio));
				const auto iNewTop = static_cast<int>(rc.top + (iDeltaY * move.m_flYRatio));
				const auto iOrigWidth = rc.right - rc.left;
				const auto iOrigHeight = rc.bottom - rc.top;
				const auto iNewWidth = static_cast<int>(iOrigWidth + (iDeltaX * size.m_flXRatio));
				const auto iNewHeight = static_cast<int>(iOrigHeight + (iDeltaY * size.m_flYRatio));
				::DeferWindowPos(hDWP, hWnd, nullptr, iNewLeft, iNewTop, iNewWidth, iNewHeight, SWP_NOZORDER | SWP_NOACTIVATE);
			}
			::EndDeferWindowPos(hDWP);
		}

		class CPoint final : public POINT {
		public:
			CPoint() : POINT { } { }
			CPoint(POINT pt) : POINT { pt } { }
			CPoint(int x, int y) : POINT { .x { x }, .y { y } } { }
			~CPoint() = default;
			operator LPPOINT() { return this; }
			operator const POINT*()const { return this; }
			bool operator==(CPoint rhs)const { return x == rhs.x && y == rhs.y; }
			bool operator==(POINT pt)const { return x == pt.x && y == pt.y; }
			friend bool operator==(POINT pt, CPoint rhs) { return rhs == pt; }
			CPoint operator+(POINT pt)const { return { x + pt.x, y + pt.y }; }
			CPoint operator-(POINT pt)const { return { x - pt.x, y - pt.y }; }
			void Offset(int iX, int iY) { x += iX; y += iY; }
			void Offset(POINT pt) { Offset(pt.x, pt.y); }
		};

		class CRect final : public RECT {
		public:
			CRect() : RECT { } { }
			CRect(int iLeft, int iTop, int iRight, int iBottom) : RECT { .left { iLeft }, .top { iTop },
				.right { iRight }, .bottom { iBottom } }
			{
			}
			CRect(RECT rc) { ::CopyRect(this, &rc); }
			CRect(LPCRECT pRC) { ::CopyRect(this, pRC); }
			CRect(POINT pt, SIZE size) : RECT { .left { pt.x }, .top { pt.y }, .right { pt.x + size.cx },
				.bottom { pt.y + size.cy } }
			{
			}
			CRect(POINT topLeft, POINT botRight) : RECT { .left { topLeft.x }, .top { topLeft.y },
				.right { botRight.x }, .bottom { botRight.y } }
			{
			}
			~CRect() = default;
			operator LPRECT() { return this; }
			operator LPCRECT()const { return this; }
			bool operator==(CRect rhs)const { return ::EqualRect(this, rhs); }
			bool operator==(RECT rc)const { return ::EqualRect(this, &rc); }
			friend bool operator==(RECT rc, CRect rhs) { return rhs == rc; }
			CRect& operator=(RECT rc) { ::CopyRect(this, &rc); return *this; }
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

		class CDC {
		public:
			CDC() = default;
			CDC(HDC hDC) : m_hDC(hDC) { }
			~CDC() = default;
			operator HDC()const { return m_hDC; }
			void AbortDoc()const { ::AbortDoc(m_hDC); }
			int AlphaBlend(int iX, int iY, int iWidth, int iHeight, HDC hDCSrc,
				int iXSrc, int iYSrc, int iWidthSrc, int iHeightSrc, BYTE bSrcAlpha = 255, BYTE bAlphaFormat = AC_SRC_ALPHA)const
			{
				const BLENDFUNCTION bf { .SourceConstantAlpha { bSrcAlpha }, .AlphaFormat { bAlphaFormat } };
				return ::AlphaBlend(m_hDC, iX, iY, iWidth, iHeight, hDCSrc, iXSrc, iYSrc, iWidthSrc, iHeightSrc, bf);
			}
			BOOL BitBlt(int iX, int iY, int iWidth, int iHeight, HDC hDCSource, int iXSource, int iYSource, DWORD dwROP)const
			{
				//When blitting from a monochrome bitmap to a color one, the black color in the monohrome bitmap 
				//becomes the destination DC’s text color, and the white color in the monohrome bitmap 
				//becomes the destination DC’s background color, when using SRCCOPY mode.
				return ::BitBlt(m_hDC, iX, iY, iWidth, iHeight, hDCSource, iXSource, iYSource, dwROP);
			}
			[[nodiscard]] auto CreateCompatibleBitmap(int iWidth, int iHeight)const -> HBITMAP
			{
				return ::CreateCompatibleBitmap(m_hDC, iWidth, iHeight);
			}
			[[nodiscard]] CDC CreateCompatibleDC()const { return ::CreateCompatibleDC(m_hDC); }
			void DeleteDC()const { ::DeleteDC(m_hDC); }
			bool DrawFrameControl(LPRECT pRC, UINT uType, UINT uState)const
			{
				return ::DrawFrameControl(m_hDC, pRC, uType, uState);
			}
			bool DrawFrameControl(int iX, int iY, int iWidth, int iHeight, UINT uType, UINT uState)const
			{
				RECT rc { .left { iX }, .top { iY }, .right { iX + iWidth }, .bottom { iY + iHeight } };
				return DrawFrameControl(&rc, uType, uState);
			}
			void DrawImage(HBITMAP hBmp, int iX, int iY, int iWidth, int iHeight)const
			{
				const auto dcMem = CreateCompatibleDC();
				dcMem.SelectObject(hBmp);
				BITMAP bm; ::GetObjectW(hBmp, sizeof(BITMAP), &bm);

				//Only 32bit bitmaps can have alpha channel.
				//If destination and source bitmaps do not have the same color format, 
				//AlphaBlend converts the source bitmap to match the destination bitmap.
				//AlphaBlend works with both, DI (DeviceIndependent) and DD (DeviceDependent), bitmaps.
				AlphaBlend(iX, iY, iWidth, iHeight, dcMem, 0, 0, iWidth, iHeight, 255, bm.bmBitsPixel == 32 ? AC_SRC_ALPHA : 0);
				dcMem.DeleteDC();
			}
			[[nodiscard]] HDC GetHDC()const { return m_hDC; }
			void GetTextMetricsW(LPTEXTMETRICW pTM)const { ::GetTextMetricsW(m_hDC, pTM); }
			auto SetBkColor(COLORREF clr)const -> COLORREF { return ::SetBkColor(m_hDC, clr); }
			void DrawEdge(LPRECT pRC, UINT uEdge, UINT uFlags)const { ::DrawEdge(m_hDC, pRC, uEdge, uFlags); }
			void DrawFocusRect(LPCRECT pRc)const { ::DrawFocusRect(m_hDC, pRc); }
			int DrawTextW(std::wstring_view wsv, LPRECT pRect, UINT uFormat)const
			{
				return DrawTextW(wsv.data(), static_cast<int>(wsv.size()), pRect, uFormat);
			}
			int DrawTextW(LPCWSTR pwszText, int iSize, LPRECT pRect, UINT uFormat)const
			{
				return ::DrawTextW(m_hDC, pwszText, iSize, pRect, uFormat);
			}
			int EndDoc()const { return ::EndDoc(m_hDC); }
			int EndPage()const { return ::EndPage(m_hDC); }
			void FillSolidRect(LPCRECT pRC, COLORREF clr)const
			{
				::SetBkColor(m_hDC, clr); ::ExtTextOutW(m_hDC, 0, 0, ETO_OPAQUE, pRC, nullptr, 0, nullptr);
			}
			[[nodiscard]] auto GetClipBox()const -> CRect { RECT rc; ::GetClipBox(m_hDC, &rc); return rc; }
			bool LineTo(POINT pt)const { return LineTo(pt.x, pt.y); }
			bool LineTo(int x, int y)const { return ::LineTo(m_hDC, x, y); }
			bool MoveTo(POINT pt)const { return MoveTo(pt.x, pt.y); }
			bool MoveTo(int x, int y)const { return ::MoveToEx(m_hDC, x, y, nullptr); }
			bool Polygon(const POINT* pPT, int iCount)const { return ::Polygon(m_hDC, pPT, iCount); }
			int SetDIBits(HBITMAP hBmp, UINT uStartLine, UINT uLines, const void* pBits, const BITMAPINFO* pBMI, UINT uClrUse)const
			{
				return ::SetDIBits(m_hDC, hBmp, uStartLine, uLines, pBits, pBMI, uClrUse);
			}
			int SetDIBitsToDevice(int iX, int iY, DWORD dwWidth, DWORD dwHeight, int iXSrc, int iYSrc, UINT uStartLine, UINT uLines,
				const void* pBits, const BITMAPINFO* pBMI, UINT uClrUse)const
			{
				return ::SetDIBitsToDevice(m_hDC, iX, iY, dwWidth, dwHeight, iXSrc, iYSrc, uStartLine, uLines, pBits, pBMI, uClrUse);
			}
			int SetMapMode(int iMode)const { return ::SetMapMode(m_hDC, iMode); }
			auto SetTextColor(COLORREF clr)const -> COLORREF { return ::SetTextColor(m_hDC, clr); }
			auto SetViewportOrg(int iX, int iY)const -> POINT { POINT pt; ::SetViewportOrgEx(m_hDC, iX, iY, &pt); return pt; }
			auto SelectObject(HGDIOBJ hObj)const -> HGDIOBJ { return ::SelectObject(m_hDC, hObj); }
			int StartDocW(const DOCINFO* pDI)const { return ::StartDocW(m_hDC, pDI); }
			int StartPage()const { return ::StartPage(m_hDC); }
			void TextOutW(int iX, int iY, LPCWSTR pwszText, int iSize)const { ::TextOutW(m_hDC, iX, iY, pwszText, iSize); }
			void TextOutW(int iX, int iY, std::wstring_view wsv)const
			{
				TextOutW(iX, iY, wsv.data(), static_cast<int>(wsv.size()));
			}
		protected:
			HDC m_hDC;
		};

		class CPaintDC final : public CDC {
		public:
			CPaintDC(HWND hWnd) : m_hWnd(hWnd) { assert(::IsWindow(hWnd)); m_hDC = ::BeginPaint(m_hWnd, &m_PS); }
			~CPaintDC() { ::EndPaint(m_hWnd, &m_PS); }
		private:
			PAINTSTRUCT m_PS;
			HWND m_hWnd;
		};

		class CMemDC final : public CDC {
		public:
			CMemDC(HDC hDC, RECT rc) : m_hDCOrig(hDC), m_rc(rc)
			{
				m_hDC = ::CreateCompatibleDC(m_hDCOrig);
				assert(m_hDC != nullptr);
				const auto iWidth = m_rc.right - m_rc.left;
				const auto iHeight = m_rc.bottom - m_rc.top;
				m_hBmp = ::CreateCompatibleBitmap(m_hDCOrig, iWidth, iHeight);
				assert(m_hBmp != nullptr);
				::SelectObject(m_hDC, m_hBmp);
			}
			~CMemDC()
			{
				const auto iWidth = m_rc.right - m_rc.left;
				const auto iHeight = m_rc.bottom - m_rc.top;
				::BitBlt(m_hDCOrig, m_rc.left, m_rc.top, iWidth, iHeight, m_hDC, m_rc.left, m_rc.top, SRCCOPY);
				::DeleteObject(m_hBmp);
				::DeleteDC(m_hDC);
			}
		private:
			HDC m_hDCOrig;
			HBITMAP m_hBmp;
			RECT m_rc;
		};

		class CWnd {
		public:
			CWnd() = default;
			CWnd(HWND hWnd) { Attach(hWnd); }
			~CWnd() = default;
			CWnd& operator=(CWnd) = delete;
			CWnd& operator=(HWND) = delete;
			operator HWND()const { return m_hWnd; }
			[[nodiscard]] bool operator==(const CWnd& rhs)const { return m_hWnd == rhs.m_hWnd; }
			[[nodiscard]] bool operator==(HWND hWnd)const { return m_hWnd == hWnd; }
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
			[[nodiscard]] int GetCheckedRadioButton(int iIDFirst, int iIDLast)const
			{
				assert(IsWindow()); for (int iID = iIDFirst; iID <= iIDLast; ++iID) {
					if (::IsDlgButtonChecked(m_hWnd, iID) != 0) { return iID; }
				} return 0;
			}
			[[nodiscard]] auto GetDC()const -> HDC { assert(IsWindow()); return ::GetDC(m_hWnd); }
			[[nodiscard]] int GetDlgCtrlID()const { assert(IsWindow()); return ::GetDlgCtrlID(m_hWnd); }
			[[nodiscard]] auto GetDlgItem(int iIDCtrl)const -> CWnd { assert(IsWindow()); return ::GetDlgItem(m_hWnd, iIDCtrl); }
			[[nodiscard]] auto GetHFont()const -> HFONT
			{
				assert(IsWindow()); return reinterpret_cast<HFONT>(::SendMessageW(m_hWnd, WM_GETFONT, 0, 0));
			}
			[[nodiscard]] auto GetHWND()const -> HWND { return m_hWnd; }
			[[nodiscard]] auto GetLogFont()const -> std::optional<LOGFONTW>
			{
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
			[[nodiscard]] auto GetWindowRect()const -> CRect {
				assert(IsWindow()); RECT rc; ::GetWindowRect(m_hWnd, &rc); return rc;
			}
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
			int MapWindowPoints(HWND hWndTo, LPRECT pRC)const
			{
				assert(IsWindow()); return ::MapWindowPoints(m_hWnd, hWndTo, reinterpret_cast<LPPOINT>(pRC), 2);
			}
			bool RedrawWindow(LPCRECT pRC = nullptr, HRGN hrgn = nullptr, UINT uFlags = RDW_INVALIDATE | RDW_UPDATENOW | RDW_ERASE)const {
				assert(IsWindow()); return static_cast<bool>(::RedrawWindow(m_hWnd, pRC, hrgn, uFlags));
			}
			int ReleaseDC(HDC hDC)const { assert(IsWindow()); return ::ReleaseDC(m_hWnd, hDC); }
			auto SetTimer(UINT_PTR uID, UINT uElapse, TIMERPROC pFN = nullptr)const -> UINT_PTR
			{
				assert(IsWindow()); return ::SetTimer(m_hWnd, uID, uElapse, pFN);
			}
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
			void SetFocus()const { assert(IsWindow()); ::SetFocus(m_hWnd); }
			void SetForegroundWindow()const { assert(IsWindow()); ::SetForegroundWindow(m_hWnd); }
			void SetScrollInfo(bool fVert, const SCROLLINFO& si)const {
				assert(IsWindow()); ::SetScrollInfo(m_hWnd, fVert, &si, TRUE);
			}
			void SetScrollPos(bool fVert, int iPos)const {
				const SCROLLINFO si { .cbSize { sizeof(SCROLLINFO) }, .fMask { SIF_POS }, .nPos { iPos } };
				SetScrollInfo(fVert, si);
			}
			void SetWindowPos(HWND hWndAfter, int iX, int iY, int iWidth, int iHeight, UINT uFlags)const {
				assert(IsWindow()); ::SetWindowPos(m_hWnd, hWndAfter, iX, iY, iWidth, iHeight, uFlags);
			}
			auto SetWndClassLong(int iIndex, LONG_PTR dwNewLong)const -> ULONG_PTR {
				assert(IsWindow()); return ::SetClassLongPtrW(m_hWnd, iIndex, dwNewLong);
			}
			void SetWndText(LPCWSTR pwszStr)const { assert(IsWindow()); ::SetWindowTextW(m_hWnd, pwszStr); }
			void SetWndText(const std::wstring& wstr)const { SetWndText(wstr.data()); }
			void SetRedraw(bool fRedraw)const { assert(IsWindow()); ::SendMessageW(m_hWnd, WM_SETREDRAW, fRedraw, 0); }
			bool ShowWindow(int iCmdShow)const { assert(IsWindow()); return ::ShowWindow(m_hWnd, iCmdShow); }
			[[nodiscard]] static auto FromHandle(HWND hWnd) -> CWnd { return hWnd; }
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

		class CMenu {
		public:
			CMenu() = default;
			CMenu(HMENU hMenu) { Attach(hMenu); }
			~CMenu() = default;
			CMenu operator=(const CWnd&) = delete;
			CMenu operator=(HMENU) = delete;
			operator HMENU()const { return m_hMenu; }
			void AppendItem(UINT uFlags, UINT_PTR uIDItem, LPCWSTR pNameItem)const
			{
				assert(IsMenu()); ::AppendMenuW(m_hMenu, uFlags, uIDItem, pNameItem);
			}
			void AppendSepar()const { AppendItem(MF_SEPARATOR, 0, nullptr); }
			void AppendString(UINT_PTR uIDItem, LPCWSTR pNameItem)const { AppendItem(MF_STRING, uIDItem, pNameItem); }
			void Attach(HMENU hMenu) { m_hMenu = hMenu; }
			void CreatePopupMenu() { Attach(::CreatePopupMenu()); }
			void DestroyMenu() { assert(IsMenu()); ::DestroyMenu(m_hMenu); m_hMenu = nullptr; }
			void Detach() { m_hMenu = nullptr; }
			void EnableItem(UINT uIDItem, bool fEnable, bool fByID = true)const
			{
				assert(IsMenu()); ::EnableMenuItem(m_hMenu, uIDItem, (fEnable ? MF_ENABLED : MF_GRAYED) |
					(fByID ? MF_BYCOMMAND : MF_BYPOSITION));
			}
			[[nodiscard]] auto GetHMENU()const -> HMENU { return m_hMenu; }
			[[nodiscard]] auto GetItemBitmap(UINT uID, bool fByID = true)const -> HBITMAP
			{
				return GetItemInfo(uID, MIIM_BITMAP, fByID).hbmpItem;
			}
			[[nodiscard]] auto GetItemBitmapCheck(UINT uID, bool fByID = true)const -> HBITMAP
			{
				return GetItemInfo(uID, MIIM_CHECKMARKS, fByID).hbmpChecked;
			}
			[[nodiscard]] auto GetItemID(int iPos)const -> UINT
			{
				assert(IsMenu()); return ::GetMenuItemID(m_hMenu, iPos);
			}
			bool GetItemInfo(UINT uID, LPMENUITEMINFOW pMII, bool fByID = true)const
			{
				assert(IsMenu()); return ::GetMenuItemInfoW(m_hMenu, uID, !fByID, pMII) != FALSE;
			}
			[[nodiscard]] auto GetItemInfo(UINT uID, UINT uMask, bool fByID = true)const -> MENUITEMINFOW
			{
				MENUITEMINFOW mii { .cbSize { sizeof(MENUITEMINFOW) }, .fMask { uMask } };
				GetItemInfo(uID, &mii, fByID); return mii;
			}
			[[nodiscard]] auto GetItemState(UINT uID, bool fByID = true)const -> UINT
			{
				assert(IsMenu()); return ::GetMenuState(m_hMenu, uID, fByID ? MF_BYCOMMAND : MF_BYPOSITION);
			}
			[[nodiscard]] auto GetItemType(UINT uID, bool fByID = true)const -> UINT
			{
				return GetItemInfo(uID, MIIM_FTYPE, fByID).fType;
			}
			[[nodiscard]] auto GetItemWstr(UINT uID, bool fByID = true)const -> std::wstring
			{
				wchar_t buff[128]; MENUITEMINFOW mii { .cbSize { sizeof(MENUITEMINFOW) }, .fMask { MIIM_STRING },
					.dwTypeData { buff }, .cch { 128 } }; return GetItemInfo(uID, &mii, fByID) ? buff : std::wstring { };
			}
			[[nodiscard]] int GetItemsCount()const
			{
				assert(IsMenu()); return ::GetMenuItemCount(m_hMenu);
			}
			[[nodiscard]] auto GetSubMenu(int iPos)const -> CMenu { assert(IsMenu()); return ::GetSubMenu(m_hMenu, iPos); };
			[[nodiscard]] bool IsItemChecked(UINT uIDItem, bool fByID = true)const
			{
				return GetItemState(uIDItem, fByID) & MF_CHECKED;
			}
			[[nodiscard]] bool IsItemSepar(UINT uPos)const { return GetItemState(uPos, false) & MF_SEPARATOR; }
			[[nodiscard]] bool IsMenu()const { return ::IsMenu(m_hMenu); }
			bool LoadMenuW(HINSTANCE hInst, LPCWSTR pwszName) { m_hMenu = ::LoadMenuW(hInst, pwszName); return IsMenu(); }
			bool LoadMenuW(HINSTANCE hInst, UINT uMenuID) { return LoadMenuW(hInst, MAKEINTRESOURCEW(uMenuID)); }
			void SetItemBitmap(UINT uItem, HBITMAP hBmp, bool fByID = true)const
			{
				const MENUITEMINFOW mii { .cbSize { sizeof(MENUITEMINFOW) }, .fMask { MIIM_BITMAP }, .hbmpItem { hBmp } };
				SetItemInfo(uItem, &mii, fByID);
			}
			void SetItemBitmapCheck(UINT uItem, HBITMAP hBmp, bool fByID = true)const
			{
				::SetMenuItemBitmaps(m_hMenu, uItem, fByID ? MF_BYCOMMAND : MF_BYPOSITION, nullptr, hBmp);
			}
			void SetItemCheck(UINT uIDItem, bool fCheck, bool fByID = true)const
			{
				assert(IsMenu()); ::CheckMenuItem(m_hMenu, uIDItem, (fCheck ? MF_CHECKED : MF_UNCHECKED) |
					(fByID ? MF_BYCOMMAND : MF_BYPOSITION));
			}
			void SetItemData(UINT uItem, ULONG_PTR dwData, bool fByID = true)const
			{
				const MENUITEMINFOW mii { .cbSize { sizeof(MENUITEMINFOW) }, .fMask { MIIM_DATA }, .dwItemData { dwData } };
				SetItemInfo(uItem, &mii, fByID);
			}
			void SetItemInfo(UINT uItem, LPCMENUITEMINFO pMII, bool fByID = true)const
			{
				assert(IsMenu()); ::SetMenuItemInfoW(m_hMenu, uItem, !fByID, pMII);
			}
			void SetItemType(UINT uItem, UINT uType, bool fByID = true)const
			{
				const MENUITEMINFOW mii { .cbSize { sizeof(MENUITEMINFOW) }, .fMask { MIIM_FTYPE }, .fType { uType } };
				SetItemInfo(uItem, &mii, fByID);
			}
			void SetItemWstr(UINT uItem, const std::wstring& wstr, bool fByID = true)const
			{
				const MENUITEMINFOW mii { .cbSize { sizeof(MENUITEMINFOW) }, .fMask { MIIM_STRING },
					.dwTypeData { const_cast<LPWSTR>(wstr.data()) } };
				SetItemInfo(uItem, &mii, fByID);
			}
			BOOL TrackPopupMenu(int iX, int iY, HWND hWndOwner, UINT uFlags = TPM_LEFTALIGN | TPM_TOPALIGN | TPM_LEFTBUTTON)const
			{
				assert(IsMenu()); return ::TrackPopupMenuEx(m_hMenu, uFlags, iX, iY, hWndOwner, nullptr);
			}
		private:
			HMENU m_hMenu { }; //Windows menu handle.
		};
	};

	enum class EFontInfo : std::int8_t { FONT_FAMILY, FONT_FACE };
	constexpr auto MSG_FONT_CHOOSE { 0x1 };

	struct FONTCHOOSE {
		NMHDR hdr { };
		UINT32 u32FamilyItemID { };
		UINT32 u32FaceItemID { };
	};


	//class CDWFontChooseEnum.

	class CDWFontChooseEnum final {
	public:
		CDWFontChooseEnum();
		~CDWFontChooseEnum();
		void Create(HWND hWndParent, UINT uCtrlID, EFontInfo eFontInfo);
		[[nodiscard]] auto ProcessMsg(const MSG& msg) -> LRESULT;
		void SetData(const std::vector<DXUT::DWFONTFAMILYINFO>& vecFontInfo, UINT32 u32ItemID = 0);
	private:
		struct ITEMSINVIEW {
			UINT32 u32FirstItem { }; //Index in the vector.
			UINT32 u32Total { };
			float flFirstItemCoordX { };
			float flFirstItemCoordY { };
		};
		template <typename T> requires std::is_arithmetic_v<T>
		[[nodiscard]] auto DIPFromPixels(T t)const -> float;
		void EnsureVisible(UINT32 u32Item)const;
		[[nodiscard]] auto GetDPIScale()const -> float;
		[[nodiscard]] auto GetItemsInView()const -> ITEMSINVIEW;
		[[nodiscard]] auto GetItemsTotal()const -> UINT32;
		[[nodiscard]] auto GetLineHeightPx()const -> int;
		[[nodiscard]] auto GetLineSpacing()const -> float;
		[[nodiscard]] auto GetScrollPosPx()const -> int;
		void ItemHighlight(UINT32 u32Item);
		void ItemSelectIncDec(UINT32 u32Lines, bool fInc);
		void ItemSelect(UINT32 u32Item);
		[[nodiscard]] bool IsDataSet()const;
		auto OnEraseBkgnd(const MSG& msg) -> LRESULT;
		auto OnGetDlgCode(const MSG& msg) -> LRESULT;
		auto OnKeyDown(const MSG& msg) -> LRESULT;
		auto OnLButtonDown(const MSG& msg) -> LRESULT;
		auto OnMouseMove(const MSG& msg) -> LRESULT;
		auto OnMouseWheel(const MSG& msg) -> LRESULT;
		auto OnPaint() -> LRESULT;
		auto OnSize(const MSG& msg) -> LRESULT;
		auto OnVScroll(const MSG& msg) -> LRESULT;
		[[nodiscard]] int PixelsFromDIP(float flDIP)const;
		void RecalcScroll();
		void ScrollLines(int iLines);
		static auto CALLBACK SubclassProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
			UINT_PTR uIDSubclass, DWORD_PTR dwRefData)->LRESULT;
	private:
		static constexpr auto m_pwszClassName { L"DWFontChooseFontEnum" };
		static constexpr auto m_flSizeFontMainDIP { 12.F };
		static constexpr auto m_flLineSpacing { m_flSizeFontMainDIP * 2.F };
		static constexpr auto m_flBaseLine { m_flLineSpacing * 0.7F };
		GDIUT::CWnd m_Wnd;
		GDIUT::CPoint m_ptMouseCurr; //Current mouse pos to avoid spurious WM_MOUSEMOVE msgs.
		DXUT::comptr<ID2D1Device> m_pD2DDevice;
		DXUT::comptr<ID2D1DeviceContext> m_pD2DDeviceContext;
		DXUT::comptr<IDXGISwapChain1> m_pDXGISwapChain;
		DXUT::comptr<ID2D1Bitmap1> m_pD2DTargetBitmap;
		DXUT::comptr<ID2D1SolidColorBrush> m_pD2DBrushBlack;
		DXUT::comptr<ID2D1SolidColorBrush> m_pD2DBrushGray;
		DXUT::comptr<ID2D1SolidColorBrush> m_pD2DBrushLightGray;
		DXUT::comptr<IDWriteTextFormat1> m_pDWTextFormatMain;
		DXUT::comptr<IDWriteTextLayout1> m_pLayoutData;
		DXUT::DWFONTINFO m_tfi { .wstrFamilyName { L"Courier New" }, .wstrLocale { L"en-us" },
			.flSizeDIP { m_flSizeFontMainDIP } };
		const std::vector<DXUT::DWFONTFAMILYINFO>* m_pVecFontInfo { };
		float m_flDPIScale { 1.F }; //DPI scale factor for the window.
		UINT32 m_u32ItemSelected { };
		UINT32 m_u32ItemHighlighted { };
		UINT32 m_u32FamilyItemID { }; //ItemID in the vector when m_eFontInfo == FONT_FACE.
		EFontInfo m_eFontInfo { };
		bool m_fDataSet { false };
	};

	CDWFontChooseEnum::CDWFontChooseEnum()
	{
		if (WNDCLASSEXW wc { }; ::GetClassInfoExW(nullptr, m_pwszClassName, &wc) == FALSE) {
			wc.cbSize = sizeof(WNDCLASSEXW);
			wc.style = CS_GLOBALCLASS | CS_DBLCLKS | CS_HREDRAW | CS_VREDRAW;
			wc.lpfnWndProc = GDIUT::WndProc<CDWFontChooseEnum>;
			wc.hCursor = static_cast<HCURSOR>(::LoadImageW(nullptr, IDC_ARROW, IMAGE_CURSOR, 0, 0, LR_DEFAULTSIZE | LR_SHARED));
			wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
			wc.lpszClassName = m_pwszClassName;
			if (::RegisterClassExW(&wc) == 0) {
				ut::DBG_REPORT(L"RegisterClassExW failed.");
				return;
			}
		}
	}

	CDWFontChooseEnum::~CDWFontChooseEnum() {
		::UnregisterClassW(m_pwszClassName, nullptr);
	}

	void CDWFontChooseEnum::Create(HWND hWndParent, UINT uCtrlID, EFontInfo eFontInfo)
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
		m_eFontInfo = eFontInfo;

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
		m_pDWTextFormatMain = DWCreateTextFormat(m_tfi);
		m_pDWTextFormatMain->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
		m_pDWTextFormatMain->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, GetLineSpacing(), m_flBaseLine);
	}

	auto CDWFontChooseEnum::ProcessMsg(const MSG& msg)->LRESULT
	{
		switch (msg.message) {
		case WM_GETDLGCODE: return OnGetDlgCode(msg);
		case WM_ERASEBKGND: return OnEraseBkgnd(msg);
		case WM_SYSKEYDOWN:
		case WM_KEYDOWN: return OnKeyDown(msg);
		case WM_LBUTTONDOWN: return OnLButtonDown(msg);
		case WM_MOUSEMOVE: return OnMouseMove(msg);
		case WM_MOUSEWHEEL: return OnMouseWheel(msg);
		case WM_PAINT: return OnPaint();
		case WM_SIZE: return OnSize(msg);
		case WM_VSCROLL: return OnVScroll(msg);
		default: return GDIUT::DefWndProc(msg);
		}
	}

	void CDWFontChooseEnum::SetData(const std::vector<DXUT::DWFONTFAMILYINFO>& vecFontInfo, UINT32 u32ItemID)
	{
		assert(u32ItemID < vecFontInfo.size());
		assert(m_Wnd.IsWindow());

		m_pVecFontInfo = &vecFontInfo;
		m_u32FamilyItemID = u32ItemID;
		m_u32ItemHighlighted = 0; //Highlight first line on data set.
		std::wstring wstrData;

		switch (m_eFontInfo) {
		case EFontInfo::FONT_FAMILY:
			for (const auto& vec : vecFontInfo) {
				wstrData += vec.wstrFamilyName + L"\r";
			}
			break;
		case EFontInfo::FONT_FACE:
			for (const auto& vec : vecFontInfo[u32ItemID].vecFontFaceInfo) {
				wstrData += vec.wstrFaceName + L"\r";
			}
			break;
		default:
			break;
		}

		if (!wstrData.empty()) {
			wstrData.pop_back(); //Remove last '\r'.
		}

		m_pLayoutData = DXUT::DWCreateTextLayout(wstrData, m_pDWTextFormatMain, 0, 0);
		m_fDataSet = true;
		RecalcScroll();
		m_Wnd.RedrawWindow();
		ItemSelect(0); //Select the very first item.
	}


	template <typename T> requires std::is_arithmetic_v<T>
	auto CDWFontChooseEnum::DIPFromPixels(T t)const->float {
		return t / GetDPIScale();
	}

	void CDWFontChooseEnum::EnsureVisible(UINT32 u32Item)const
	{
		const auto iScrollYPx = GetScrollPosPx();
		const auto iLineHeightPx = GetLineHeightPx();
		const auto rcClient = m_Wnd.GetClientRect();
		const int iItemPos = u32Item * iLineHeightPx;
		int iPosScrollNew = 0xFFFFFFFF;

		if (iItemPos < iScrollYPx) {
			iPosScrollNew = iItemPos;
		}
		else if (iItemPos >= (iScrollYPx + rcClient.Height())) {
			iPosScrollNew = iItemPos - rcClient.Height() + iLineHeightPx;
		}

		if (iPosScrollNew != 0xFFFFFFFF) {
			m_Wnd.SetScrollPos(true, iPosScrollNew);
		}

		m_Wnd.RedrawWindow();
	}

	auto CDWFontChooseEnum::GetDPIScale()const->float {
		return m_flDPIScale;
	}

	auto CDWFontChooseEnum::GetItemsInView()const->ITEMSINVIEW
	{
		const auto iScrollYPx = GetScrollPosPx();
		const auto iLineHeightPx = GetLineHeightPx();
		const auto rcClient = m_Wnd.GetClientRect();
		const UINT32 u32FirstItem = iScrollYPx / iLineHeightPx;
		UINT32 u32ItemsInView = (rcClient.Height() / iLineHeightPx) + (rcClient.Height() % iLineHeightPx > 0 ? 1 : 0);

		std::size_t sSizeMax { 0U };
		switch (m_eFontInfo) {
		case EFontInfo::FONT_FAMILY:
			sSizeMax = m_pVecFontInfo->size();
			break;
		case EFontInfo::FONT_FACE:
			sSizeMax = (*m_pVecFontInfo)[m_u32FamilyItemID].vecFontFaceInfo.size();
			break;
		default:
			break;
		}

		if (u32ItemsInView + u32FirstItem > sSizeMax) {
			u32ItemsInView = static_cast<UINT32>(sSizeMax - u32FirstItem);
		}

		return { .u32FirstItem { u32FirstItem }, .u32Total { u32ItemsInView },
			.flFirstItemCoordX { DIPFromPixels(rcClient.Width() - (rcClient.Width() / 3)) },
			.flFirstItemCoordY { DIPFromPixels(iScrollYPx % iLineHeightPx) } };
	}

	auto CDWFontChooseEnum::GetItemsTotal()const->UINT32
	{
		if (!IsDataSet()) {
			return { };
		}

		switch (m_eFontInfo) {
		case EFontInfo::FONT_FAMILY:
			return static_cast<UINT32>(m_pVecFontInfo->size() - 1);
		case EFontInfo::FONT_FACE:
			if ((*m_pVecFontInfo)[m_u32FamilyItemID].vecFontFaceInfo.empty()) {
				return { };
			}

			return static_cast<UINT32>((*m_pVecFontInfo)[m_u32FamilyItemID].vecFontFaceInfo.size() - 1);
		default:
			return { };
		}
	}

	auto CDWFontChooseEnum::GetLineHeightPx()const->int {
		return PixelsFromDIP(GetLineSpacing());
	}

	auto CDWFontChooseEnum::GetLineSpacing()const->float {
		return m_flLineSpacing;
	}

	auto CDWFontChooseEnum::GetScrollPosPx()const->int {
		return m_Wnd.GetScrollPos(true);
	}

	void CDWFontChooseEnum::ItemHighlight(UINT32 u32Item)
	{
		if (!IsDataSet()) {
			return;
		}

		switch (m_eFontInfo) {
		case EFontInfo::FONT_FAMILY:
			if (u32Item >= m_pVecFontInfo->size()) {
				u32Item = static_cast<UINT32>(m_pVecFontInfo->size() - 1);
			}
			break;
		case EFontInfo::FONT_FACE:
			if (u32Item >= (*m_pVecFontInfo)[m_u32FamilyItemID].vecFontFaceInfo.size()) {
				u32Item = static_cast<UINT32>((*m_pVecFontInfo)[m_u32FamilyItemID].vecFontFaceInfo.size() - 1);
			}
			break;
		default:
			break;
		}

		m_u32ItemHighlighted = u32Item;
		m_Wnd.RedrawWindow();
	}

	void CDWFontChooseEnum::ItemSelectIncDec(UINT32 u32Items, bool fInc)
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

		ItemSelect(u32ItemToSelect);
		EnsureVisible(u32ItemToSelect);
	}

	void CDWFontChooseEnum::ItemSelect(UINT32 u32Item)
	{
		if (!IsDataSet()) {
			return;
		}

		UINT32 u32FamilyItemID { };
		UINT32 u32FaceItemID { };
		switch (m_eFontInfo) {
		case EFontInfo::FONT_FAMILY:
			if (u32Item >= m_pVecFontInfo->size()) {
				u32Item = static_cast<UINT32>(m_pVecFontInfo->size() - 1);
			}
			u32FamilyItemID = u32Item;
			u32FaceItemID = 0;
			break;
		case EFontInfo::FONT_FACE:
			if (u32Item >= (*m_pVecFontInfo)[m_u32FamilyItemID].vecFontFaceInfo.size()) {
				u32Item = static_cast<UINT32>((*m_pVecFontInfo)[m_u32FamilyItemID].vecFontFaceInfo.size() - 1);
			}
			u32FamilyItemID = m_u32FamilyItemID;
			u32FaceItemID = u32Item;
			break;
		default:
			break;
		}

		m_u32ItemSelected = u32Item;
		FONTCHOOSE fc { .hdr { .hwndFrom { m_Wnd }, .idFrom { static_cast<UINT_PTR>(m_Wnd.GetDlgCtrlID()) },
			.code { MSG_FONT_CHOOSE } }, .u32FamilyItemID { u32FamilyItemID }, .u32FaceItemID { u32FaceItemID } };
		m_Wnd.GetParent().SendMsg(WM_NOTIFY, 0, reinterpret_cast<LPARAM>(&fc));
		m_Wnd.RedrawWindow();
	}

	bool CDWFontChooseEnum::IsDataSet()const {
		return m_fDataSet;
	}

	auto CDWFontChooseEnum::OnEraseBkgnd([[maybe_unused]] const MSG& msg)->LRESULT {
		return 1;
	}

	auto CDWFontChooseEnum::OnGetDlgCode([[maybe_unused]] const MSG& msg)->LRESULT {
		return DLGC_WANTALLKEYS;
	}

	auto CDWFontChooseEnum::OnKeyDown(const MSG& msg)->LRESULT
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
			ItemSelectIncDec(GetItemsInView().u32Total, false);
			break;
		case VK_NEXT:
			ItemSelectIncDec(GetItemsInView().u32Total, true);
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

	auto CDWFontChooseEnum::OnLButtonDown(const MSG& msg)->LRESULT
	{
		const POINT pt { .x { ut::GetXLPARAM(msg.lParam) }, .y { ut::GetYLPARAM(msg.lParam) } };
		m_Wnd.SetFocus();
		ItemSelect(m_u32ItemHighlighted);

		return 0;
	}

	auto CDWFontChooseEnum::OnMouseMove(const MSG& msg)->LRESULT
	{
		const POINT pt { .x { ut::GetXLPARAM(msg.lParam) }, .y { ut::GetYLPARAM(msg.lParam) } };
		if (m_ptMouseCurr == pt) {
			return 0;
		}

		m_ptMouseCurr = pt;
		ItemHighlight(static_cast<UINT32>((pt.y + GetScrollPosPx()) / GetLineHeightPx()));

		return 0;
	}

	auto CDWFontChooseEnum::OnMouseWheel(const MSG& msg)->LRESULT
	{
		const auto iDelta = GET_WHEEL_DELTA_WPARAM(msg.wParam);
		ScrollLines(iDelta > 0 ? -2 : 2);

		return 0;
	}

	auto CDWFontChooseEnum::OnPaint()->LRESULT
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
		const auto flHighlightedBkY = (m_u32ItemHighlighted * GetLineSpacing()) - iScrollYDIP;
		const auto rcHighlightedBk = D2D1::RectF(0, flHighlightedBkY, DIPFromPixels(rcClient.Width()),
			flHighlightedBkY + GetLineSpacing());
		m_pD2DDeviceContext->DrawRectangle(rcHighlightedBk, m_pD2DBrushGray, 1); //Highlighted item.
		const auto flSelectedBkY = (m_u32ItemSelected * GetLineSpacing()) - iScrollYDIP;
		const auto rcSelectedBk = D2D1::RectF(0, flSelectedBkY, DIPFromPixels(rcClient.Width()),
			flSelectedBkY + GetLineSpacing());
		m_pD2DDeviceContext->FillRectangle(rcSelectedBk, m_pD2DBrushLightGray); //Selected item.
		m_pD2DDeviceContext->DrawTextLayout(D2D1::Point2F(10.F, -iScrollYDIP), m_pLayoutData, m_pD2DBrushBlack);

		for (auto uLine = 0U; uLine < liv.u32Total; ++uLine) {
			const auto uCurrLine = liv.u32FirstItem + uLine;
			const DXUT::DWFONTFAMILYINFO* pFontInfo { };
			const DXUT::DWFONTFACEINFO* pFontFaceInfo { };
			switch (m_eFontInfo) {
			case EFontInfo::FONT_FAMILY:
				pFontInfo = &(*m_pVecFontInfo)[uCurrLine];
				pFontFaceInfo = &(pFontInfo->vecFontFaceInfo)[0];
				break;
			case EFontInfo::FONT_FACE:
				pFontInfo = &(*m_pVecFontInfo)[m_u32FamilyItemID];
				pFontFaceInfo = &(pFontInfo->vecFontFaceInfo)[uCurrLine];
				break;
			default:
				break;
			}

			const auto eWeight = static_cast<DWRITE_FONT_WEIGHT>(std::stoi(pFontFaceInfo->wstrWeight));
			const auto eStretch = static_cast<DWRITE_FONT_STRETCH>(std::stoi(pFontFaceInfo->wstrStretch));
			const auto eStyle = static_cast<DWRITE_FONT_STYLE>(std::stoi(pFontFaceInfo->wstrStyle));
			const DXUT::DWFONTINFO tfiSample { .wstrFamilyName { pFontInfo->wstrFamilyName },
				.wstrLocale { pFontInfo->wstrLocale }, .eWeight { eWeight }, .eStretch { eStretch }, .eStyle { eStyle },
				.flSizeDIP { m_flSizeFontMainDIP * 1.5F } };
			DXUT::comptr pFormatSample = DXUT::DWCreateTextFormat(tfiSample);
			pFormatSample->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
			pFormatSample->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, GetLineSpacing(), m_flBaseLine);
			const auto pLayoutSample = DXUT::DWCreateTextLayout(L"Sample", pFormatSample, 0, 0);
			const auto flY = (static_cast<float>(uLine) * GetLineSpacing()) - liv.flFirstItemCoordY;
			m_pD2DDeviceContext->DrawTextLayout(D2D1::Point2F(liv.flFirstItemCoordX, flY), pLayoutSample, m_pD2DBrushBlack);
		}

		m_pD2DDeviceContext->EndDraw();
		m_pDXGISwapChain->Present(0, 0);

		return 0;
	}

	auto CDWFontChooseEnum::OnSize(const MSG& msg)->LRESULT
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

	auto CDWFontChooseEnum::OnVScroll(const MSG& msg)->LRESULT
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

	auto CDWFontChooseEnum::PixelsFromDIP(float flDIP)const->int {
		return std::lround(flDIP * GetDPIScale());
	}

	void CDWFontChooseEnum::RecalcScroll()
	{
		if (!IsDataSet()) {
			return;
		}

		DWRITE_TEXT_METRICS tm;
		m_pLayoutData->GetMetrics(&tm);
		const auto iMax = PixelsFromDIP(tm.height);
		auto si = m_Wnd.GetScrollInfo(true);
		si.nPos = (iMax > si.nPos) ? si.nPos : iMax;
		si.nMax = iMax;
		si.nPage = m_Wnd.GetClientRect().Height();
		m_Wnd.SetScrollInfo(true, si);
	}

	void CDWFontChooseEnum::ScrollLines(int iLines)
	{
		m_Wnd.SetScrollPos(true, GetScrollPosPx() + (iLines * GetLineHeightPx()));
		m_Wnd.RedrawWindow();
	}

	auto CDWFontChooseEnum::SubclassProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
		UINT_PTR uIDSubclass, [[maybe_unused]] DWORD_PTR dwRefData) -> LRESULT
	{
		if (uMsg == WM_NCDESTROY) {
			::RemoveWindowSubclass(hWnd, SubclassProc, uIDSubclass);
		}

		const auto pCtrl = reinterpret_cast<CDWFontChooseEnum*>(uIDSubclass);
		return pCtrl->ProcessMsg({ .hwnd { hWnd }, .message { uMsg }, .wParam { wParam }, .lParam { lParam } });
	}


	//class CDWFontChooseSampleText.

	class CDWFontChooseSampleText final {
	public:
		CDWFontChooseSampleText();
		~CDWFontChooseSampleText();
		void Create(HWND hWndParent, UINT uCtrlID);
		[[nodiscard]] auto ProcessMsg(const MSG& msg) -> LRESULT;
		void SetFontInfo(const DXUT::DWFONTINFO& fi);
	private:
		template <typename T> requires std::is_arithmetic_v<T>
		[[nodiscard]] auto DIPFromPixels(T t)const -> float;
		[[nodiscard]] auto GetDPIScale()const -> float;
		auto OnEraseBkgnd(const MSG& msg) -> LRESULT;
		auto OnGetDlgCode(const MSG& msg) -> LRESULT;
		auto OnLButtonDown(const MSG& msg) -> INT_PTR;
		auto OnMouseWheel(const MSG& msg) -> INT_PTR;
		auto OnPaint() -> LRESULT;
		auto OnSize(const MSG& msg) -> LRESULT;
		auto OnVScroll(const MSG& msg) -> LRESULT;
		[[nodiscard]] int PixelsFromDIP(float flDIP)const;
		void RecalcScroll();
		static auto CALLBACK SubclassProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
			UINT_PTR uIDSubclass, DWORD_PTR dwRefData)->LRESULT;
	private:
		static constexpr auto m_pwszClassName { L"DWFontChooseSampleText" };
		static constexpr auto m_flSizeFontMainDIP { 20.F };
		GDIUT::CWnd m_Wnd;
		DXUT::comptr<ID2D1Device> m_pD2DDevice;
		DXUT::comptr<ID2D1DeviceContext> m_pD2DDeviceContext;
		DXUT::comptr<IDXGISwapChain1> m_pDXGISwapChain;
		DXUT::comptr<ID2D1Bitmap1> m_pD2DTargetBitmap;
		DXUT::comptr<ID2D1SolidColorBrush> m_pD2DBrushBlack;
		DXUT::comptr<IDWriteTextLayout1> m_pLayoutData;
		DXUT::CTextEffect m_effSelect;
		DXUT::CDWriteTextRenderer m_D2DTextRenderer;
		float m_flDPIScale { 1.F }; //DPI scale factor for the window.
	};

	CDWFontChooseSampleText::CDWFontChooseSampleText() {
		if (WNDCLASSEXW wc { }; ::GetClassInfoExW(nullptr, m_pwszClassName, &wc) == FALSE) {
			wc.cbSize = sizeof(WNDCLASSEXW);
			wc.style = CS_GLOBALCLASS | CS_DBLCLKS | CS_HREDRAW | CS_VREDRAW;
			wc.lpfnWndProc = GDIUT::WndProc<CDWFontChooseEnum>;
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
	}

	auto CDWFontChooseSampleText::ProcessMsg(const MSG& msg)->LRESULT
	{
		switch (msg.message) {
		case WM_ERASEBKGND: return OnEraseBkgnd(msg);
		case WM_GETDLGCODE: return OnGetDlgCode(msg);
		case WM_LBUTTONDOWN: return OnLButtonDown(msg);
		case WM_MOUSEWHEEL: return OnMouseWheel(msg);
		case WM_PAINT: return OnPaint();
		case WM_SIZE: return OnSize(msg);
		case WM_VSCROLL: return OnVScroll(msg);
		default: return GDIUT::DefWndProc(msg);
		}
	}

	void CDWFontChooseSampleText::SetFontInfo(const DXUT::DWFONTINFO& fi)
	{
		const auto pTextFormat = DWCreateTextFormat(fi);
		m_D2DTextRenderer.SetDrawContext({ .pDeviceContext { m_pD2DDeviceContext }, .pTextLayout { m_pLayoutData },
			.pBrushTextDef { m_pD2DBrushBlack } });
		const auto rcClient = m_Wnd.GetClientRect();
		m_pLayoutData = DXUT::DWCreateTextLayout(L"The quick brown fox jumps over the lazy dog.", pTextFormat,
			DIPFromPixels(rcClient.Width()), DIPFromPixels(rcClient.Height()));
		m_pLayoutData->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
		m_pLayoutData->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
		RecalcScroll();
		m_Wnd.RedrawWindow();
	}


	template <typename T> requires std::is_arithmetic_v<T>
	auto CDWFontChooseSampleText::DIPFromPixels(T t)const->float {
		return t / GetDPIScale();
	}

	auto CDWFontChooseSampleText::GetDPIScale()const->float {
		return m_flDPIScale;
	}

	auto CDWFontChooseSampleText::OnEraseBkgnd([[maybe_unused]] const MSG& msg)->LRESULT {
		return 1;
	}

	auto CDWFontChooseSampleText::OnGetDlgCode([[maybe_unused]] const MSG& msg)->LRESULT {
		return DLGC_WANTALLKEYS;
	}

	auto CDWFontChooseSampleText::OnLButtonDown([[maybe_unused]] const MSG& msg)->LRESULT
	{
		m_Wnd.SetFocus();
		return 0;
	}

	auto CDWFontChooseSampleText::OnMouseWheel(const MSG& msg)->LRESULT
	{
		const auto iDelta = GET_WHEEL_DELTA_WPARAM(msg.wParam);
		const auto iScrollY = (m_Wnd.GetClientRect().Height() / 4) * (iDelta > 0 ? -1 : 1);
		m_Wnd.SetScrollPos(true, m_Wnd.GetScrollPos(true) + iScrollY);
		m_Wnd.RedrawWindow();

		return 0;
	}

	auto CDWFontChooseSampleText::OnPaint()->LRESULT
	{
		::ValidateRect(m_Wnd, nullptr);

		m_pD2DDeviceContext->BeginDraw();
		m_pD2DDeviceContext->Clear(D2D1::ColorF(D2D1::ColorF::White));

		if (m_pLayoutData) {
			m_pLayoutData->Draw(nullptr, &m_D2DTextRenderer, 0, -DIPFromPixels(m_Wnd.GetScrollPos(true)));
		}

		m_pD2DDeviceContext->EndDraw();
		m_pDXGISwapChain->Present(0, 0);

		return 0;
	}

	auto CDWFontChooseSampleText::OnSize(const MSG& msg)->LRESULT
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

		if (!m_pLayoutData) {
			return 0;
		}

		m_pLayoutData->SetMaxWidth(DIPFromPixels(wWidth));
		RecalcScroll();

		return 0;
	}

	auto CDWFontChooseSampleText::OnVScroll(const MSG& msg)->LRESULT
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

	int CDWFontChooseSampleText::PixelsFromDIP(float flDIP)const {
		return std::lround(flDIP * GetDPIScale());
	}

	void CDWFontChooseSampleText::RecalcScroll()
	{
		DWRITE_TEXT_METRICS tm;
		m_pLayoutData->GetMetrics(&tm);
		const auto rcClient = m_Wnd.GetClientRect();
		m_pLayoutData->SetMaxHeight((std::max)(tm.height, DIPFromPixels(rcClient.Height())));
		const auto iMax = PixelsFromDIP(tm.height);
		auto si = m_Wnd.GetScrollInfo(true);
		si.nPos = (iMax > si.nPos) ? si.nPos : iMax;
		si.nMax = iMax;
		si.nPage = rcClient.Height();
		m_Wnd.SetScrollInfo(true, si);
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
		[[nodiscard]] auto GetData() -> DXUT::DWFONTINFO&;
		[[nodiscard]] auto ProcessMsg(const MSG& msg) -> INT_PTR;
	private:
		void EditSizeIncDec(int iSizeToAdd);
		[[nodiscard]] auto GetEditFontSize()const -> float;
		[[nodiscard]] auto GetFontInfo() -> DXUT::DWFONTINFO;
		void FontFaceChoosen(const FONTCHOOSE* pFontChoose);
		void OnCancel();
		auto OnCommand(const MSG& msg) -> INT_PTR;
		void OnCommandCombo(DWORD dwCtrlID, DWORD dwCode);
		void OnCommandEdit(DWORD dwCtrlID, DWORD dwCode);
		auto OnDestroy() -> INT_PTR;
		auto OnInitDialog(const MSG& msg) -> INT_PTR;
		auto OnLButtonDown(const MSG& msg) -> INT_PTR;
		auto OnLButtonUp(const MSG& msg) -> INT_PTR;
		auto OnMouseMove(const MSG& msg) -> INT_PTR;
		auto OnNotify(const MSG& msg) -> INT_PTR;
		void OnOK();
		auto OnSize(const MSG& msg) -> INT_PTR;
		void SetComboWeightSel(DWORD_PTR dwWeight);
		void SetComboStretchSel(DWORD_PTR dwStretch);
		void SetComboStyleSel(DWORD_PTR dwStyle);
		void SetEditFontSize(float flSize);
		void UpdateSampleText();
	private:
		static inline auto m_hCurResize { static_cast<HCURSOR>(::LoadImageW(nullptr, IDC_SIZEWE, IMAGE_CURSOR, 0, 0, LR_SHARED)) };
		static inline auto m_hCurArrow { static_cast<HCURSOR>(::LoadImageW(nullptr, IDC_ARROW, IMAGE_CURSOR, 0, 0, LR_SHARED)) };
		DWFONTCHOOSEINFO m_fci;
		GDIUT::CWnd m_Wnd;
		GDIUT::CDynLayout m_DynLayout;
		GDIUT::CWndEdit m_EditSize;
		GDIUT::CWndCombo m_ComboWeight;
		GDIUT::CWndCombo m_ComboStretch;
		GDIUT::CWndCombo m_ComboStyle;
		std::vector<DXUT::DWFONTFAMILYINFO> m_vecFonts;
		DXUT::DWFONTINFO m_FI;
		CDWFontChooseEnum m_FontFamilies;
		CDWFontChooseEnum m_FontFaces;
		CDWFontChooseSampleText m_SampleText;
		UINT32 m_u32FamilyItemID { }; //Currently choosen font family in the m_vecFonts.
		UINT32 m_u32FaceItemID { };   //Currently choosen font face in the m_vecFonts[m_u32FamilyItemID].
		GDIUT::CPoint m_ptSizeClicked;
		bool m_fLMDownSize { };
	};

	auto CDWFontChooseDlg::DoModal(const DWFONTCHOOSEINFO& fci)->INT_PTR
	{
		m_fci = fci;
		return ::DialogBoxParamW(fci.hInstRes, MAKEINTRESOURCEW(IDD_DWFONTCHOOSE),
			fci.hWndParent, GDIUT::DlgProc<CDWFontChooseDlg>, reinterpret_cast<LPARAM>(this));
	}

	auto CDWFontChooseDlg::GetData()->DXUT::DWFONTINFO& {
		return m_FI;
	}

	auto CDWFontChooseDlg::ProcessMsg(const MSG& msg)->INT_PTR
	{
		switch (msg.message) {
		case WM_COMMAND: return OnCommand(msg);
		case WM_DESTROY: return OnDestroy();
		case WM_INITDIALOG: return OnInitDialog(msg);
		case WM_LBUTTONDOWN: return OnLButtonDown(msg);
		case WM_LBUTTONUP: return OnLButtonUp(msg);
		case WM_MOUSEMOVE: return OnMouseMove(msg);
		case WM_NOTIFY: return OnNotify(msg);
		case WM_SIZE: return OnSize(msg);
		default:
			return 0;
		}
	}


	void CDWFontChooseDlg::EditSizeIncDec(int iSizeToAdd) {
		SetEditFontSize(GetEditFontSize() + iSizeToAdd);
	}

	void CDWFontChooseDlg::FontFaceChoosen(const FONTCHOOSE* pFontChoose)
	{
		if (pFontChoose == nullptr) {
			return;
		}

		m_u32FamilyItemID = pFontChoose->u32FamilyItemID;
		m_u32FaceItemID = pFontChoose->u32FaceItemID;
		const auto& Family = m_vecFonts[m_u32FamilyItemID];
		const auto& Face = Family.vecFontFaceInfo[m_u32FaceItemID];
		SetComboWeightSel(std::stoul(Face.wstrWeight));
		SetComboStretchSel(std::stoul(Face.wstrStretch));
		SetComboStyleSel(std::stoul(Face.wstrStyle));
		UpdateSampleText();
	}

	auto CDWFontChooseDlg::GetEditFontSize()const->float {
		return std::clamp(m_EditSize.IsWndTextEmpty() ? 1.F : std::stof(m_EditSize.GetWndText()), 1.F, 1296.F);
	}

	auto CDWFontChooseDlg::GetFontInfo()->DXUT::DWFONTINFO
	{
		const auto& wstrFamily = m_vecFonts[m_u32FamilyItemID].wstrFamilyName;
		const auto eWeight = static_cast<DWRITE_FONT_WEIGHT>(m_ComboWeight.GetItemData(m_ComboWeight.GetCurSel()));
		const auto eStretch = static_cast<DWRITE_FONT_STRETCH>(m_ComboStretch.GetItemData(m_ComboStretch.GetCurSel()));
		const auto eStyle = static_cast<DWRITE_FONT_STYLE>(m_ComboStyle.GetItemData(m_ComboStyle.GetCurSel()));
		const auto flSizeDIP = ut::FontPixelsFromPoints(GetEditFontSize());
		return { .wstrFamilyName { wstrFamily }, .wstrLocale { m_fci.wstrLocale }, .eWeight { eWeight },
			.eStretch { eStretch }, .eStyle { eStyle }, .flSizeDIP { flSizeDIP } };
	}

	auto CDWFontChooseDlg::OnCommand(const MSG& msg)->INT_PTR
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
		case IDC_COMBO_FONT_WEIGHT:
		case IDC_COMBO_FONT_STRETCH:
		case IDC_COMBO_FONT_STYLE:
			OnCommandCombo(uCtrlID, uCode);
			break;
		case IDC_EDIT_FONT_SIZE:
			OnCommandEdit(uCtrlID, uCode);
			break;
		default:
			return FALSE;
		}

		return TRUE;
	}

	void CDWFontChooseDlg::OnCommandCombo([[maybe_unused]] DWORD dwCtrlID, DWORD dwCode)
	{
		if (dwCode == CBN_SELCHANGE) {
			UpdateSampleText();
		}
	}

	void CDWFontChooseDlg::OnCommandEdit([[maybe_unused]] DWORD dwCtrlID, DWORD dwCode)
	{
		if (dwCode == EN_CHANGE) {
			UpdateSampleText();
		}
	}

	auto CDWFontChooseDlg::OnDestroy()->INT_PTR
	{
		m_vecFonts.clear();
		return TRUE;
	};

	auto CDWFontChooseDlg::OnInitDialog(const MSG& msg)->INT_PTR
	{
		m_Wnd.Attach(msg.hwnd);
		m_EditSize.Attach(m_Wnd.GetDlgItem(IDC_EDIT_FONT_SIZE));
		m_ComboWeight.Attach(m_Wnd.GetDlgItem(IDC_COMBO_FONT_WEIGHT));
		m_ComboStretch.Attach(m_Wnd.GetDlgItem(IDC_COMBO_FONT_STRETCH));
		m_ComboStyle.Attach(m_Wnd.GetDlgItem(IDC_COMBO_FONT_STYLE));

		auto iIndex = m_ComboWeight.AddString(L"Thin");
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
		m_vecFonts = DXUT::DWGetSystemFonts(m_fci.wstrLocale.data());
		std::sort(m_vecFonts.begin(), m_vecFonts.end(), [](DXUT::DWFONTFAMILYINFO& lhs, DXUT::DWFONTFAMILYINFO& rhs) {
			return lhs.wstrFamilyName < rhs.wstrFamilyName;	});
		m_FontFamilies.Create(m_Wnd, IDC_CUSTOM_FONT_FAMILY, EFontInfo::FONT_FAMILY);
		m_FontFaces.Create(m_Wnd, IDC_CUSTOM_FONT_FACE, EFontInfo::FONT_FACE);
		m_FontFamilies.SetData(m_vecFonts);
		SetEditFontSize(35);

		m_DynLayout.SetHost(m_Wnd);
		m_DynLayout.AddItem(IDC_CUSTOM_FONT_FAMILY, GDIUT::CDynLayout::MoveNone(), GDIUT::CDynLayout::SizeHorzAndVert(50, 50));
		m_DynLayout.AddItem(IDC_CUSTOM_FONT_FACE, GDIUT::CDynLayout::MoveHorz(50), GDIUT::CDynLayout::SizeHorzAndVert(50, 50));
		m_DynLayout.AddItem(IDC_CUSTOM_FONT_SAMPLE, GDIUT::CDynLayout::MoveVert(50), GDIUT::CDynLayout::SizeHorzAndVert(100, 50));
		m_DynLayout.AddItem(IDC_EDIT_FONT_SIZE, GDIUT::CDynLayout::MoveVert(100), GDIUT::CDynLayout::SizeNone());
		m_DynLayout.AddItem(IDC_STATIC_SIZE, GDIUT::CDynLayout::MoveVert(100), GDIUT::CDynLayout::SizeNone());
		m_DynLayout.AddItem(IDC_COMBO_FONT_WEIGHT, GDIUT::CDynLayout::MoveVert(100), GDIUT::CDynLayout::SizeNone());
		m_DynLayout.AddItem(IDC_STATIC_WEIGHT, GDIUT::CDynLayout::MoveVert(100), GDIUT::CDynLayout::SizeNone());
		m_DynLayout.AddItem(IDC_COMBO_FONT_STRETCH, GDIUT::CDynLayout::MoveVert(100), GDIUT::CDynLayout::SizeNone());
		m_DynLayout.AddItem(IDC_STATIC_STRETCH, GDIUT::CDynLayout::MoveVert(100), GDIUT::CDynLayout::SizeNone());
		m_DynLayout.AddItem(IDC_COMBO_FONT_STYLE, GDIUT::CDynLayout::MoveVert(100), GDIUT::CDynLayout::SizeNone());
		m_DynLayout.AddItem(IDC_STATIC_STYLE, GDIUT::CDynLayout::MoveVert(100), GDIUT::CDynLayout::SizeNone());
		m_DynLayout.AddItem(IDC_BTN_OK, GDIUT::CDynLayout::MoveHorzAndVert(100, 100), GDIUT::CDynLayout::SizeNone());
		m_DynLayout.AddItem(IDCANCEL, GDIUT::CDynLayout::MoveHorzAndVert(100, 100), GDIUT::CDynLayout::SizeNone());
		m_DynLayout.Enable(true);

		return TRUE;
	}

	auto CDWFontChooseDlg::OnLButtonDown(const MSG& msg)->LRESULT
	{
		const POINT pt { .x { ut::GetXLPARAM(msg.lParam) }, .y { ut::GetYLPARAM(msg.lParam) } };
		auto rcStaticSize = m_Wnd.GetDlgItem(IDC_STATIC_SIZE).GetWindowRect();
		m_Wnd.ScreenToClient(rcStaticSize);
		if (rcStaticSize.PtInRect(pt)) {
			m_Wnd.SetCapture();
			m_ptSizeClicked = pt;
			m_fLMDownSize = true;
			::SetCursor(m_hCurResize);
		}

		return TRUE;
	}

	auto CDWFontChooseDlg::OnLButtonUp(const MSG& msg)->LRESULT
	{
		const POINT pt { .x { ut::GetXLPARAM(msg.lParam) }, .y { ut::GetYLPARAM(msg.lParam) } };
		m_fLMDownSize = false;
		::ReleaseCapture();

		return TRUE;
	}

	auto CDWFontChooseDlg::OnMouseMove(const MSG& msg)->LRESULT
	{
		const POINT pt { .x { ut::GetXLPARAM(msg.lParam) }, .y { ut::GetYLPARAM(msg.lParam) } };

		if (m_fLMDownSize) {
			::SetCursor(m_hCurResize);
			EditSizeIncDec(pt.x - m_ptSizeClicked.x); //Positive or negative.
			m_ptSizeClicked.x = pt.x;
		}
		else {
			auto rcStaticSize = GDIUT::CWnd::FromHandle(m_Wnd.GetDlgItem(IDC_STATIC_SIZE)).GetWindowRect();
			m_Wnd.ScreenToClient(rcStaticSize);
			::SetCursor(rcStaticSize.PtInRect(pt) ? m_hCurResize : m_hCurArrow);
		}

		return TRUE;
	}

	auto CDWFontChooseDlg::OnNotify(const MSG& msg)->INT_PTR
	{
		const auto pFC = reinterpret_cast<FONTCHOOSE*>(msg.lParam);
		const auto pNMHDR = &pFC->hdr;

		switch (pNMHDR->idFrom) {
		case IDC_CUSTOM_FONT_FAMILY:
			switch (pNMHDR->code) {
			case MSG_FONT_CHOOSE:
				m_FontFaces.SetData(m_vecFonts, pFC->u32FamilyItemID);
				break;
			default:
				break;
			}
			break;
		case IDC_CUSTOM_FONT_FACE:
			switch (pNMHDR->code) {
			case MSG_FONT_CHOOSE:
				FontFaceChoosen(pFC);
				break;
			default:
				break;
			}
			break;
		default:
			break;
		}

		return TRUE;
	}

	void CDWFontChooseDlg::OnOK()
	{
		m_FI = GetFontInfo();
		m_Wnd.EndDialog(IDOK);
	}

	auto CDWFontChooseDlg::OnSize(const MSG& msg)->LRESULT
	{
		const auto wWidth = LOWORD(msg.lParam);
		const auto wHeight = HIWORD(msg.lParam);
		m_DynLayout.OnSize(wWidth, wHeight);

		return TRUE;
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

	void CDWFontChooseDlg::UpdateSampleText()
	{
		if (m_vecFonts.empty()) {
			return;
		}

		m_SampleText.SetFontInfo(GetFontInfo());
	}
};

export [[nodiscard]] auto DWFontChoose(const DWFONTCHOOSEINFO& fci) -> std::optional<DXUT::DWFONTINFO> {
	DWFONTCHOOSE::CDWFontChooseDlg dlg;
	return dlg.DoModal(fci) == IDOK ? std::optional { dlg.GetData() } : std::nullopt;
}

export [[nodiscard]] auto DWFontChoose() -> std::optional<DXUT::DWFONTINFO> {
	return DWFontChoose({ });
}