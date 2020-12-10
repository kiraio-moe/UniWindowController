// LibUniWinC.cpp

#include "pch.h"
#include "libuniwinc.h"


static HWND hTargetWnd_ = NULL;
static WINDOWINFO originalWindowInfo;
static WINDOWPLACEMENT originalWindowPlacement;
static SIZE szOriginaiBorder_;
static POINT ptVirtualScreen_;
static SIZE szVirtualScreen_;
static INT nPrimaryMonitorHeight_;
static BOOL bIsTransparent_ = FALSE;
static BOOL bIsBorderless_ = FALSE;
static BOOL bIsTopmost_ = FALSE;
static BOOL bIsClickThrough_ = FALSE;
static BOOL bAllowDropFile_ = FALSE;
static COLORREF dwKeyColor_ = 0x00000000;		// AABBGGRR
static TransparentType nTransparentType_ = TransparentType::Alpha;
static TransparentType nCurrentTransparentType_ = TransparentType::Alpha;
static INT nMonitorCount_ = 0;							// モニタ数。モニタ解像度一覧取得時は一時的に0に戻る
static RECT pMonitorRect_[UNIWINC_MAX_MONITORCOUNT];	// EnumDisplayMonitorsの順番で保持した、各画面のRECT
static INT pMonitorIndices_[UNIWINC_MAX_MONITORCOUNT];	// このライブラリ独自のモニタ番号をキーとした、EnumDisplayMonitorsでの順番
static HMONITOR hMonitors_[UNIWINC_MAX_MONITORCOUNT];	// Monitor handles
static WNDPROC lpMyWndProc_ = NULL;
static WNDPROC lpOriginalWndProc_ = NULL;
static HHOOK hHook_ = NULL;
static DropFilesCallback hDropFilesHandler_ = nullptr;
static MonitorChangedCallback hDisplayChangedHandler_ = nullptr;

void attachWindow(const HWND hWnd);
void detachWindow();
void refreshWindow();
void updateScreenSize();
void BeginHook();
void EndHook();
void CreateCustomWindowProcedure();
void DestroyCustomWindowProcedure();


/// <summary>
/// 既にウィンドウが選択済みなら、元の状態に戻して選択を解除
/// </summary>
void detachWindow()
{
	if (hTargetWnd_) {
		// Restore the original window procedure
		DestroyCustomWindowProcedure();

		//// Unhook if exist
		//EndHook();

		if (IsWindow(hTargetWnd_)) {
			SetTransparent(false);

			SetWindowLong(hTargetWnd_, GWL_STYLE, originalWindowInfo.dwStyle);
			SetWindowLong(hTargetWnd_, GWL_EXSTYLE, originalWindowInfo.dwExStyle);

			SetWindowPlacement(hTargetWnd_, &originalWindowPlacement);

			refreshWindow();
		}
	}
	hTargetWnd_ = NULL;
}

/// <summary>
/// 指定ハンドルのウィンドウを今後使うようにする
/// </summary>
/// <param name="hWnd"></param>
void attachWindow(const HWND hWnd) {
	// 選択済みウィンドウが異なるものであれば、元に戻す
	if (hTargetWnd_ != hWnd) {
		detachWindow();
	}

	// とりあえずこのタイミングで画面サイズも更新
	//   本来は画面解像度変更時に更新したい。ウィンドウプロシージャでどう？
	updateScreenSize();

	// Set the target
	hTargetWnd_ = hWnd;

	if (hWnd) {
		// Save the original state
		GetWindowInfo(hWnd, &originalWindowInfo);
		GetWindowPlacement(hWnd, &originalWindowPlacement);

		// Apply current settings
		SetTransparent(bIsTransparent_);
		SetBorderless(bIsBorderless_);
		SetTopmost(bIsTopmost_);
		SetClickThrough(bIsClickThrough_);
		SetAllowDrop(bAllowDropFile_);

		// Replace the window procedure
		CreateCustomWindowProcedure();
	}
}

/// <summary>
/// オーナーウィンドウハンドルを探す際のコールバック
/// </summary>
/// <param name="hWnd"></param>
/// <param name="lParam"></param>
/// <returns></returns>
BOOL CALLBACK findOwnerWindowProc(const HWND hWnd, const LPARAM lParam)
{
	DWORD currentPid = (DWORD)lParam;
	DWORD pid;
	GetWindowThreadProcessId(hWnd, &pid);

	// プロセスIDが一致すれば自分のウィンドウとする
	if (pid == currentPid) {

		// オーナーウィンドウを探す
		// Unityエディタだと本体が選ばれて独立Gameビューが選ばれない…
		HWND hOwner = GetWindow(hWnd, GW_OWNER);
		if (hOwner) {
			// あればオーナーを選択
			attachWindow(hOwner);
		}
		else {
			// オーナーが無ければこのウィンドウを選択
			attachWindow(hWnd);
		}
		return FALSE;

		//// 同じプロセスIDでも、表示されているウィンドウのみを選択
		//LONG style = GetWindowLong(hWnd, GWL_STYLE);
		//if (style & WS_VISIBLE) {
		//	hTargetWnd_ = hWnd;
		//	return FALSE;
		//}
	}

	return TRUE;
}

/// <summary>
/// モニタ情報取得時のコールバック
/// EnumDisplayMonitors()で呼ばれる。その際は最初にnMonitorCountが0にセットされるものとする。
/// </summary>
/// <param name="hMon"></param>
/// <param name="hDc"></param>
/// <param name="lpRect"></param>
/// <param name="lParam"></param>
/// <returns></returns>
BOOL CALLBACK monitorEnumProc(HMONITOR hMon, HDC hDc, LPRECT lpRect, LPARAM lParam)
{
	// 最大取り扱いモニタ数に達したら探索終了
	if (nMonitorCount_ >= UNIWINC_MAX_MONITORCOUNT) return FALSE;

	// RECTを記憶
	pMonitorRect_[nMonitorCount_] = *lpRect;

	// インデックスを一旦登場順で保存
	pMonitorIndices_[nMonitorCount_] = nMonitorCount_;

	// Store the monitor handle
	hMonitors_[nMonitorCount_] = hMon;

	// モニタ数カウント
	nMonitorCount_++;

	return TRUE;
}

/// <summary>
/// 接続モニタ数とそれらのサイズ一覧を取得
/// </summary>
/// <returns>成功ならTRUE</returns>
BOOL updateMonitorRectangles() {
	//  カウントするため一時的に0に戻す
	nMonitorCount_ = 0;

	// モニタを列挙してRECTを保存
	if (!EnumDisplayMonitors(NULL, NULL, monitorEnumProc, NULL)) {
		return FALSE;
	}

	// モニタの位置を基準にバブルソート
	for (int i = 0; i < (nMonitorCount_ - 1); i++) {
		for (int j = (nMonitorCount_ - 1); j > i; j--) {
			RECT pr = pMonitorRect_[pMonitorIndices_[j - 1]];
			RECT cr = pMonitorRect_[pMonitorIndices_[j]];

			// 左にあるモニタが先、横が同じなら下にあるモニタが先となるようソート
			if (pr.left >  cr.left || ((pr.left == cr.left) && (pr.bottom < cr.bottom))) {
				int index = pMonitorIndices_[j - 1];
				pMonitorIndices_[j - 1] = pMonitorIndices_[j];
				pMonitorIndices_[j] = index;
			}
		}
	}

	return TRUE;
}

void enableTransparentByDWM()
{
	if (!hTargetWnd_) return;

	// 全面をGlassにする
	MARGINS margins = { -1 };
	DwmExtendFrameIntoClientArea(hTargetWnd_, &margins);
}

void disableTransparentByDWM()
{
	if (!hTargetWnd_) return;

	// 枠のみGlassにする
	//	※ 本来のウィンドウが何らかの範囲指定でGlassにしていた場合は、残念ながら表示が戻りません
	MARGINS margins = { 0, 0, 0, 0 };
	DwmExtendFrameIntoClientArea(hTargetWnd_, &margins);
}

/// <summary>
/// SetLayeredWindowsAttributes によって指定色を透過させる
/// </summary>
void enableTransparentBySetLayered()
{
	if (!hTargetWnd_) return;

	LONG exstyle = GetWindowLong(hTargetWnd_, GWL_EXSTYLE);
	exstyle |= WS_EX_LAYERED;
	SetWindowLong(hTargetWnd_, GWL_EXSTYLE, exstyle);
	SetLayeredWindowAttributes(hTargetWnd_, dwKeyColor_, 0xFF, LWA_COLORKEY);
}

/// <summary>
/// SetLayeredWindowsAttributes による指定色透過を解除
/// </summary>
void disableTransparentBySetLayered()
{
	COLORREF cref = { 0 };
	SetLayeredWindowAttributes(hTargetWnd_, cref, 0xFF, LWA_ALPHA);

	LONG exstyle = originalWindowInfo.dwExStyle;
	//exstyle &= ~WinApi.WS_EX_LAYERED;
	SetWindowLong(hTargetWnd_, GWL_EXSTYLE, exstyle);
}

/// <summary>
/// 枠を消した際に描画サイズが合わなくなることに対応するため、ウィンドウを強制リサイズして更新
/// </summary>
void refreshWindow() {
	if (!hTargetWnd_) return;

	if (IsZoomed(hTargetWnd_)) {
		// 最大化されていた場合は、ウィンドウサイズ変更の代わりに一度最小化して再度最大化
		ShowWindow(hTargetWnd_, SW_MINIMIZE);
		ShowWindow(hTargetWnd_, SW_MAXIMIZE);
	}
	else if (IsIconic(hTargetWnd_)) {
		// 最小化されていた場合は、次に表示されるときにこうしんされるものとして、何もしない
	}
	else if (IsWindowVisible(hTargetWnd_)) {
		// 通常のウィンドウだった場合は、ウィンドウサイズを1px変えることで再描画

		// 現在のウィンドウサイズを取得
		RECT rect;
		GetWindowRect(hTargetWnd_, &rect);

		// 1px横幅を広げて、リサイズイベントを強制的に起こす
		SetWindowPos(
			hTargetWnd_,
			NULL,
			0, 0, (rect.right - rect.left + 1), (rect.bottom - rect.top + 1),
			SWP_NOMOVE | SWP_NOZORDER | SWP_FRAMECHANGED | SWP_NOOWNERZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS
		);

		// 元のサイズに戻す。この時もリサイズイベントは発生するはず
		SetWindowPos(
			hTargetWnd_,
			NULL,
			0, 0, (rect.right - rect.left), (rect.bottom - rect.top),
			SWP_NOMOVE | SWP_NOZORDER | SWP_FRAMECHANGED | SWP_NOOWNERZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS
		);

		ShowWindow(hTargetWnd_, SW_SHOW);
	}
}

BOOL compareRect(const RECT rcA, const RECT rcB) {
	return ((rcA.left == rcB.left) && (rcA.right == rcB.right) && (rcA.top == rcB.top) && (rcA.bottom == rcB.bottom));
}

/// <summary>
/// Update current monitor information
/// </summary>
/// <returns></returns>
void updateScreenSize() {
	ptVirtualScreen_.x = GetSystemMetrics(SM_XVIRTUALSCREEN);
	ptVirtualScreen_.y = GetSystemMetrics(SM_YVIRTUALSCREEN);
	szVirtualScreen_.cx = GetSystemMetrics(SM_CXVIRTUALSCREEN);
	szVirtualScreen_.cy = GetSystemMetrics(SM_CYVIRTUALSCREEN);
	nPrimaryMonitorHeight_ = GetSystemMetrics(SM_CYSCREEN);

	// Update the monitor resolution list.
	//   To use the nPrimaryMonitorHeight, do this after its acquisition.
	updateMonitorRectangles();
}

/// <summary>
/// Cocoaと同様のY座標に変換
/// 事前にプライマリーモニターの高さが取得できていることとする
/// </summary>
/// <param name="y"></param>
/// <returns></returns>
LONG calcFlippedY(LONG y) {
	return (nPrimaryMonitorHeight_ - y);
}

// Windows only

/// <summary>
/// 現在選択されているウィンドウハンドルを取得
/// </summary>
/// <returns></returns>
HWND UNIWINC_API GetWindowHandle() {
	return hTargetWnd_;
}

/// <summary>
/// 自分のプロセスIDを取得
/// </summary>
/// <returns></returns>
DWORD UNIWINC_API GetMyProcessId() {
	return GetCurrentProcessId();
}


// ==================================================================
// Public functions

/// <summary>
/// 利用可能な状態ならtrueを返す
/// </summary>
/// <returns></returns>
BOOL UNIWINC_API IsActive() {
	if (hTargetWnd_ && IsWindow(hTargetWnd_)) {
		return TRUE;
	}
	return FALSE;
}

/// <summary>
/// 透過にしているか否かを返す
/// </summary>
/// <returns></returns>
BOOL UNIWINC_API IsTransparent() {
	return bIsTransparent_;
}

/// <summary>
/// 枠を消去しているか否かを返す
/// </summary>
/// <returns></returns>
BOOL UNIWINC_API IsBorderless() {
	return bIsBorderless_;
}

/// <summary>
/// 最前面にしているか否かを返す
/// </summary>
/// <returns></returns>
BOOL UNIWINC_API IsTopmost() {
	return bIsTopmost_;
}

/// <summary>
/// Return true if the window is zoomed
/// </summary>
/// <returns></returns>
BOOL UNIWINC_API IsMaximized() {
	return (hTargetWnd_ && IsZoomed(hTargetWnd_));
}

/// <summary>
/// Return true if the window is iconic
/// </summary>
/// <returns></returns>
BOOL UNIWINC_API IsMinimized() {
	return (hTargetWnd_ && IsIconic(hTargetWnd_));
}

/// <summary>
/// Restore and release the target window
/// </summary>
/// <returns></returns>
BOOL UNIWINC_API DetachWindow() {
	detachWindow();
	return true;
}

/// <summary>
/// Find my own window and attach (Same as the AttachMyOwnerWindow)
/// </summary>
/// <returns></returns>
BOOL UNIWINC_API AttachMyWindow() {
	return AttachMyOwnerWindow();
}

/// <summary>
/// Find and select the window with the current process ID
/// </summary>
/// <returns></returns>
BOOL UNIWINC_API AttachMyOwnerWindow() {
	DWORD currentPid = GetCurrentProcessId();
	return EnumWindows(findOwnerWindowProc, (LPARAM)currentPid);
}

/// <summary>
/// Find and select the active window with the current process ID
///   (To attach the process with multiple windows)
/// </summary>
/// <returns></returns>
BOOL UNIWINC_API AttachMyActiveWindow() {
	DWORD currentPid = GetCurrentProcessId();
	HWND hWnd = GetActiveWindow();
	DWORD pid;

	GetWindowThreadProcessId(hWnd, &pid);
	if (pid == currentPid) {
		attachWindow(hWnd);
		return TRUE;
	}
	return FALSE;
}

/// <summary>
/// Select the transparentize method
/// </summary>
/// <param name="type"></param>
/// <returns></returns>
void UNIWINC_API SetTransparentType(const TransparentType type) {
	if (bIsTransparent_) {
		// 透明化状態であれば、一度解除してから設定
		SetTransparent(FALSE);
		nTransparentType_ = type;
		SetTransparent(TRUE);
	}
	else {
		// 透明化状態でなければ、そのまま設定
		nTransparentType_ = type;
	}
}

/// <summary>
/// 単色透過時に透過とする色を指定
/// </summary>
/// <param name="color">透過する色</param>
/// <returns></returns>
void UNIWINC_API SetKeyColor(const COLORREF color) {
	if (bIsTransparent_ && (nTransparentType_ == TransparentType::ColorKey)) {
		// 透明化状態であれば、一度解除してから設定
		SetTransparent(FALSE);
		dwKeyColor_ = color;
		SetTransparent(TRUE);
	}
	else {
		// 透明化状態でなければ、そのまま設定
		dwKeyColor_ = color;
	}
}

/// <summary>
/// 透過および枠消しを設定／解除
/// </summary>
/// <param name="bTransparent"></param>
/// <returns></returns>
void UNIWINC_API SetTransparent(const BOOL bTransparent) {
	if (hTargetWnd_) {
		if (bTransparent) {
			switch (nTransparentType_)
			{
			case TransparentType::Alpha:
				enableTransparentByDWM();
				break;
			case TransparentType::ColorKey:
				enableTransparentBySetLayered();
				break;
			default:
				break;
			}
		}
		else {
			switch (nCurrentTransparentType_)
			{
			case TransparentType::Alpha:
				disableTransparentByDWM();
				break;
			case TransparentType::ColorKey:
				disableTransparentBySetLayered();
				break;
			default:
				break;
			}
		}

		// 戻す方法を決めるため、透明化が変更された時のタイプを記憶
		nCurrentTransparentType_ = nTransparentType_;
	}

	// 透明化状態を記憶
	bIsTransparent_ = bTransparent;
}


/// <summary>
/// ウィンドウ枠を有効／無効にする
/// </summary>
/// <param name="bBorderless"></param>
void UNIWINC_API SetBorderless(const BOOL bBorderless) {
	if (hTargetWnd_) {
		int newW, newH, newX, newY;
		RECT rcWin, rcCli;
		GetWindowRect(hTargetWnd_, &rcWin);
		GetClientRect(hTargetWnd_, &rcCli);

		int w = rcWin.right - rcWin.left;
		int h = rcWin.bottom - rcWin.top;

		int bZoomed = IsZoomed(hTargetWnd_);
		int bIconic = IsIconic(hTargetWnd_);

		// 最大化されていたら、一度最大化は解除
		if (bZoomed) {
			ShowWindow(hTargetWnd_, SW_NORMAL);
		}

		if (bBorderless) {
			// 枠無しウィンドウにする
			LONG currentWS = (WS_VISIBLE | WS_POPUP);
			SetWindowLong(hTargetWnd_, GWL_STYLE, currentWS);

			newW = rcCli.right - rcCli.left;
			newH = rcCli.bottom - rcCli.top;

			int bw = (w - newW) / 2;	// 枠の片側幅 [px]
			newX = rcWin.left + bw;
			newY = rcWin.top + ((h - newH) - bw);	// 本来は枠の下側高さと左右の幅が同じ保証はないが、とりあえず同じとみなしている

		}
		else {
			// ウィンドウスタイルを戻す
			SetWindowLong(hTargetWnd_, GWL_STYLE, originalWindowInfo.dwStyle);

			int dx = (originalWindowInfo.rcWindow.right - originalWindowInfo.rcWindow.left) - (originalWindowInfo.rcClient.right - originalWindowInfo.rcClient.left);
			int dy = (originalWindowInfo.rcWindow.bottom - originalWindowInfo.rcWindow.top) - (originalWindowInfo.rcClient.bottom - originalWindowInfo.rcClient.top);
			int bw = dx / 2;	// 枠の片側幅 [px]

			newW = rcCli.right - rcCli.left + dx;
			newH = rcCli.bottom- rcCli.top + dy;

			newX = rcWin.left - bw;
			newY = rcWin.top - (dy - bw);	// 本来は枠の下側高さと左右の幅が同じ保証はないが、とりあえず同じとみなしている
		}

		// ウィンドウサイズが変化しないか、最大化や最小化状態なら標準のサイズ更新
		if (bZoomed) {
			// 最大化されていたら、ここで再度最大化
			ShowWindow(hTargetWnd_, SW_MAXIMIZE);
		} else if (bIconic) {
			// 最小化されていたら、次に表示されるときの再描画を期待して、何もしない
		} else if (newW == w && newH == h) {
			// ウィンドウ再描画
			refreshWindow();
		}
		else
		{
			// クライアント領域サイズを維持するようサイズと位置を調整
			SetWindowPos(
				hTargetWnd_,
				NULL,
				newX, newY, newW, newH,
				SWP_NOZORDER | SWP_FRAMECHANGED | SWP_NOOWNERZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS
			);

			ShowWindow(hTargetWnd_, SW_SHOW);
		}
	}

	// 枠無しかを記憶
	bIsBorderless_ = bBorderless;
}

/// <summary>
/// 最前面化／解除
/// </summary>
/// <param name="bTopmost"></param>
/// <returns></returns>
void UNIWINC_API SetTopmost(const BOOL bTopmost) {
	if (hTargetWnd_) {
		SetWindowPos(
			hTargetWnd_,
			(bTopmost ? HWND_TOPMOST : HWND_NOTOPMOST),
			0, 0, 0, 0,
			SWP_NOSIZE | SWP_NOMOVE | SWP_NOOWNERZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS // | SWP_FRAMECHANGED
		);
	}

	bIsTopmost_ = bTopmost;
}

/// <summary>
/// 最大化／解除
/// </summary>
/// <param name="bZoomed"></param>
/// <returns></returns>
void UNIWINC_API SetMaximized(const BOOL bZoomed) {
	if (hTargetWnd_) {
		if (bZoomed) {
			ShowWindow(hTargetWnd_, SW_MAXIMIZE);
		}
		else
		{
			ShowWindow(hTargetWnd_, SW_NORMAL);
		}
	}
}

/// <summary>
/// クリックスルー（マウス操作無効化）を設定／解除
/// </summary>
/// <param name="bTransparent"></param>
/// <returns></returns>
void UNIWINC_API SetClickThrough(const BOOL bTransparent) {
	if (hTargetWnd_) {
		if (bTransparent) {
			LONG exstyle = GetWindowLong(hTargetWnd_, GWL_EXSTYLE);
			exstyle |= WS_EX_TRANSPARENT;
			exstyle |= WS_EX_LAYERED;
			SetWindowLong(hTargetWnd_, GWL_EXSTYLE, exstyle);
		}
		else
		{
			LONG exstyle = GetWindowLong(hTargetWnd_, GWL_EXSTYLE);
			exstyle &= ~WS_EX_TRANSPARENT;
			if (!bIsTransparent_ && !(originalWindowInfo.dwExStyle & WS_EX_LAYERED)) {
				exstyle &= ~WS_EX_LAYERED;
			}
			SetWindowLong(hTargetWnd_, GWL_EXSTYLE, exstyle);
		}
	}
	bIsClickThrough_ = bTransparent;
}

/// <summary>
/// ウィンドウ座標を設定
/// </summary>
/// <param name="x">ウィンドウ左端座標 [px]</param>
/// <param name="y">プライマリー画面下端を原点とし、上が正のY座標 [px]</param>
/// <returns>成功すれば true</returns>
BOOL UNIWINC_API SetPosition(const float x, const float y) {
	if (hTargetWnd_ == NULL) return FALSE;

	// 現在のウィンドウ位置とサイズを取得
	RECT rect;
	GetWindowRect(hTargetWnd_, &rect);

	// 引数の y はCocoa相当の座標系でウィンドウ左下なので、変換
	int newY = (nPrimaryMonitorHeight_ - (int)y) - (rect.bottom - rect.top);

	return SetWindowPos(
		hTargetWnd_, NULL,
		(int)x, (int)newY,
		0, 0,
		SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOSIZE | SWP_NOZORDER | SWP_ASYNCWINDOWPOS
		);
}

/// <summary>
/// ウィンドウ座標を取得
/// </summary>
/// <param name="x">ウィンドウ左端座標 [px]</param>
/// <param name="y">プライマリー画面下端を原点とし、上が正のY座標 [px]</param>
/// <returns>成功すれば true</returns>
BOOL UNIWINC_API GetPosition(float* x, float* y) {
	*x = 0;
	*y = 0;

	if (hTargetWnd_ == NULL) return FALSE;

	RECT rect;
	if (GetWindowRect(hTargetWnd_, &rect)) {
		*x = (float)rect.left;
		*y = (float)(nPrimaryMonitorHeight_ - rect.bottom);	// 左下基準とする
		return TRUE;
	}
	return FALSE;
}

/// <summary>
/// ウィンドウサイズを設定
/// </summary>
/// <param name="width">幅 [px]</param>
/// <param name="height">高さ [px]</param>
/// <returns>成功すれば true</returns>
BOOL UNIWINC_API SetSize(const float width, const float height) {
	if (hTargetWnd_ == NULL) return FALSE;

	// 現在のウィンドウ位置とサイズを取得
	RECT rect;
	GetWindowRect(hTargetWnd_, &rect);

	// 左下原点とするために調整した、新規Y座標
	int newY = rect.bottom - (int)height;

	return SetWindowPos(
		hTargetWnd_, NULL,
		rect.left, newY,
		(int)width, (int)height,
		SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOZORDER | SWP_FRAMECHANGED | SWP_ASYNCWINDOWPOS
	);
}

/// <summary>
/// ウィンドウサイズを取得
/// </summary>
/// <param name="width">幅 [px]</param>
/// <param name="height">高さ [px]</param>
/// <returns>成功すれば true</returns>
BOOL  UNIWINC_API GetSize(float* width, float* height) {
	*width = 0;
	*height = 0;

	if (hTargetWnd_ == NULL) return FALSE;

	RECT rect;
	if (GetWindowRect(hTargetWnd_, &rect)) {
		*width = (float)(rect.right - rect.left);	// +1 は不要なよう
		*height = (float)(rect.bottom - rect.top);	// +1 は不要なよう
		return TRUE;
	}
	return FALSE;
}

/// <summary>
/// このウィンドウが現在表示されているモニタ番号を取得
/// </summary>
/// <returns></returns>
INT32 UNIWINC_API GetCurrentMonitor() {
	int primaryIndex = 0;

	//  ウィンドウ未取得ならプライマリモニタを探す
	if (hTargetWnd_ == NULL) {
		for (int i = 0; i < nMonitorCount_; i++) {
			RECT mr = pMonitorRect_[pMonitorIndices_[i]];

			// 原点にあるモニタはプライマリと判定
			if (mr.left == 0 && mr.top == 0) {
				primaryIndex = i;
				break;
			}
		}
		return primaryIndex;
	}

	// 現在のウィンドウの中心座標を取得
	RECT rect;
	GetWindowRect(hTargetWnd_, &rect);
	LONG cx = (rect.right - 1 + rect.left) / 2;
	LONG cy = (rect.bottom - 1 + rect.top) / 2;

	// ウィンドウの中央が含まれているモニタを検索
	for (int i = 0; i < nMonitorCount_; i++) {
		RECT mr = pMonitorRect_[pMonitorIndices_[i]];

		// ウィンドウ中心が入っていればその画面番号を返して終了
		if (mr.left <= cx && cx < mr.right && mr.top <= cy && cy < mr.bottom) {
			return i;
		}

		// 原点にあるモニタはプライマリと判定
		if (mr.left == 0 && mr.top == 0) {
			primaryIndex = i;
		}
	}

	// 判定できなければプライマリモニタの画面番号を返す
	return primaryIndex;
}


/// <summary>
/// 接続されているモニタ数を取得
/// </summary>
/// <returns>モニタ数</returns>
INT32  UNIWINC_API GetMonitorCount() {
	//// SM_CMONITORS では表示されているモニタのみ対象となる（EnumDisplayとは異なる）
	//return GetSystemMetrics(SM_CMONITORS);
	return nMonitorCount_;
}

/// <summary>
/// モニタの位置、サイズを取得
/// </summary>
/// <param name="width">幅 [px]</param>
/// <param name="height">高さ [px]</param>
/// <returns>成功すれば true</returns>
BOOL  UNIWINC_API GetMonitorRectangle(const INT32 monitorIndex, float* x, float* y, float* width, float* height) {
	*x = 0;
	*y = 0;
	*width = 0;
	*height = 0;

	if (monitorIndex < 0 || monitorIndex >= nMonitorCount_) {
		return FALSE;
	}

	RECT rect = pMonitorRect_[pMonitorIndices_[monitorIndex]];
	*x = (float)(rect.left);
	*y = (float)(nPrimaryMonitorHeight_ - rect.bottom);		// 左下基準とする
	*width = (float)(rect.right - rect.left);
	*height = (float)(rect.bottom - rect.top);
	return TRUE;
}

/// <summary>
/// Register the callback fucnction called when updated monitor information
/// </summary>
/// <param name="callback"></param>
/// <returns></returns>
BOOL UNIWINC_API RegisterMonitorChangedCallback(MonitorChangedCallback callback) {
	if (callback == nullptr) return FALSE;

	hDisplayChangedHandler_ = callback;
	return TRUE;
}

/// <summary>
/// Unregister the callback function
/// </summary>
/// <returns></returns>
BOOL UNIWINC_API UnregisterMonitorChangedCallback() {
	hDisplayChangedHandler_ = nullptr;
	return TRUE;
}



// ==================================================================
// Mouse cursor

/// <summary>
/// マウスカーソル座標を取得
/// </summary>
/// <param name="x">ウィンドウ左端座標 [px]</param>
/// <param name="y">プライマリー画面下端を原点とし、上が正のY座標 [px]</param>
/// <returns>成功すれば true</returns>
BOOL UNIWINC_API GetCursorPosition(float* x, float* y) {
	*x = 0;
	*y = 0;

	POINT pos;
	if (GetCursorPos(&pos)) {
		*x = (float)pos.x;
		*y = (float)(nPrimaryMonitorHeight_ - pos.y - 1);	// 左下基準とする
		return TRUE;
	}
	return FALSE;

}

/// <summary>
/// マウスカーソル座標を設定
/// </summary>
/// <param name="x">ウィンドウ左端座標 [px]</param>
/// <param name="y">プライマリー画面下端を原点とし、上が正のY座標 [px]</param>
/// <returns>成功すれば true</returns>
BOOL UNIWINC_API SetCursorPosition(const float x, const float y) {
	POINT pos;

	pos.x = (int)x;
	pos.y = nPrimaryMonitorHeight_ - (int)y - 1;

	return SetCursorPos(pos.x, pos.y);
}


// ==================================================================
// Shell32


/// <summary>
/// Process drop files
/// </summary>
/// <param name="hDrop"></param>
/// <returns></returns>
BOOL ReceiveDropFiles(HDROP hDrop) {
	UINT num = DragQueryFile(hDrop, 0xFFFFFFFF, NULL, 0);

	if (num > 0) {
		// Retrieve total buffer size
		UINT bufferSize = 0;
		for (UINT i = 0; i < num; i++) {
			UINT size = DragQueryFile(hDrop, i, NULL, 0);
			bufferSize += size + sizeof(L'\n');		// Add a delimiter size
		}
		bufferSize++;

		// Allocate buffer
		LPWSTR buffer;
		buffer = new (std::nothrow)WCHAR[bufferSize];

		if (buffer != NULL) {
			// Retrieve file paths
			UINT bufferIndex = 0;
			for (UINT i = 0; i < num; i++) {
				UINT cch = bufferSize - 1 - bufferIndex;
				UINT size = DragQueryFile(hDrop, i, buffer + bufferIndex, cch);
				bufferIndex += size;
				buffer[bufferIndex] = L'\n';	// Delimiter of each path
				bufferIndex++;
			}
			buffer[bufferIndex] = NULL;

			// Do callback function
			if (hDropFilesHandler_ != nullptr) {
				hDropFilesHandler_((WCHAR*)buffer);	// Charset of this project must be set U
			}

			delete[] buffer;
		}
	}

	return (num > 0);
}

/// <summary>
/// Custom window proceture to accept dropped files and display-changed event
/// </summary>
/// <param name="hWnd"></param>
/// <param name="uMsg"></param>
/// <param name="wParam"></param>
/// <param name="lParam"></param>
/// <returns></returns>
LRESULT CALLBACK CustomWindowProcedure(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	if (uMsg == WM_DROPFILES)
	{
		HDROP hDrop = (HDROP)wParam;
		ReceiveDropFiles(hDrop);
		DragFinish(hDrop);
	}
	else if (uMsg == WM_DISPLAYCHANGE) {
		updateScreenSize();

		// Do callback
		if (hDisplayChangedHandler_ != nullptr) {
			INT32 count = GetMonitorCount();
			hDisplayChangedHandler_(count);
		}
	}

	if (lpOriginalWndProc_ != NULL) {
		return CallWindowProc(lpOriginalWndProc_, hWnd, uMsg, wParam, lParam);
	}
	else {
		return DefWindowProc(hWnd, uMsg, wParam, lParam);
	}
}

/// <summary>
/// Callback when received WM_DROPFILE message
/// </summary>
/// <param name="nCode"></param>
/// <param name="wParam"></param>
/// <param name="lParam"></param>
/// <returns></returns>
LRESULT CALLBACK MessageHookCallback(int nCode, WPARAM wParam, LPARAM lParam) {
	if (nCode < 0) {
		return CallNextHookEx(NULL, nCode, wParam, lParam);
	}

	// lParam is a pointer to an MSG structure for WH_GETMESSAGE
	LPMSG msg = (LPMSG)lParam;

	if (msg->message == WM_DROPFILES) {
		if (hTargetWnd_ != NULL && msg->hwnd == hTargetWnd_) {
			HDROP hDrop = (HDROP)msg->wParam;
			ReceiveDropFiles(hDrop);
			DragFinish(hDrop);
		}
		return TRUE;
	}
	else if (msg->message == WM_DISPLAYCHANGE) {
		updateScreenSize();
	}
	return CallNextHookEx(NULL, nCode, wParam, lParam);
}

/// <summary>
/// Wrapper of SetWindowLongPtr to set window procedure
/// </summary>
/// <param name="wndProc"></param>
/// <returns></returns>
WNDPROC SetWindowProcedure(WNDPROC wndProc) {
	//return (WNDPROC)SetWindowLongPtr(hTargetWnd_, GWLP_WNDPROC, (LONG_PTR)wndProc);

#ifdef _WIN64
	// 64bit
	return (WNDPROC)SetWindowLongPtr(hTargetWnd_, GWLP_WNDPROC, (LONG_PTR)wndProc);
#else
	return (WNDPROC)SetWindowLong(hTargetWnd_, GWLP_WNDPROC, (LONG)wndProc);
#endif
}

/// <summary>
/// Remove the custom window procedure
/// </summary>
void DestroyCustomWindowProcedure() {
	if (lpMyWndProc_ == NULL) return;

	if (lpOriginalWndProc_ != NULL) {
		if (hTargetWnd_ != NULL && IsWindow(hTargetWnd_)) {
			SetWindowProcedure(lpOriginalWndProc_);
		}
		lpOriginalWndProc_ = NULL;
	}
	lpMyWndProc_ = NULL;
}

/// <summary>
/// Create and attach the custom window procedure
/// </summary>
void CreateCustomWindowProcedure() {
	if (lpMyWndProc_ != NULL) {
		DestroyCustomWindowProcedure();
	}

	if (hTargetWnd_ != NULL) {
		lpMyWndProc_ = CustomWindowProcedure;
		lpOriginalWndProc_ = SetWindowProcedure(lpMyWndProc_);
	}
}

/// <summary>
/// Set the hook
/// </summary>
void BeginHook() {
	if (hTargetWnd_ == NULL) return;

	// Return if the hook is already set
	if (hHook_ != NULL) return;

	//HMODULE hMod = GetModuleHandle(NULL);
	DWORD dwThreadId = GetCurrentThreadId();

	hHook_ = SetWindowsHookEx(WH_GETMESSAGE, MessageHookCallback, NULL, dwThreadId);
}

/// <summary>
/// Unset the hook
/// </summary>
void EndHook() {
	if (hTargetWnd_ == NULL) return;

	// Return if the hook is not set
	if (hHook_ == NULL) return;

	UnhookWindowsHookEx(hHook_);
	hHook_ = NULL;
}

/// <summary>
/// Set window procedure.
/// This method is not implemented in user32.dll
/// </summary>
/// <returns>Previous window procedure</returns>
BOOL UNIWINC_API SetAllowDrop(const BOOL bEnabled)
{
	if (hTargetWnd_ == NULL) return FALSE;

	bAllowDropFile_ = bEnabled;
	DragAcceptFiles(hTargetWnd_, bAllowDropFile_);

	if (bEnabled && (lpMyWndProc_ == NULL)) {
		// Create the window procedure if the first time
		CreateCustomWindowProcedure();
	}

	//if (bEnabled && hHook == NULL) {
	//	BeginHook();
	//}
	////else if (!bEnabled && hHook != NULL) {
	////	EndHook();
	////}

	return TRUE;
}

/// <summary>
/// Register the callback fucnction for dropping files
/// </summary>
/// <param name="callback"></param>
/// <returns></returns>
BOOL UNIWINC_API RegisterDropFilesCallback(DropFilesCallback callback) {
	if (callback == nullptr) return FALSE;

	hDropFilesHandler_ = callback;
	return TRUE;
}

/// <summary>
/// Unregister the callback function
/// </summary>
/// <returns></returns>
BOOL UNIWINC_API UnregisterDropFilesCallback() {
	hDropFilesHandler_ = nullptr;
	return TRUE;
}
