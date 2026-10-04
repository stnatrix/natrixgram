/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "platform/win/integration_win.h"

#include "base/platform/win/base_windows_winrt.h"
#include "core/application.h"
#include "core/core_settings.h"
#include "core/sandbox.h"
#include "lang/lang_keys.h"
#include "platform/win/windows_app_user_model_id.h"
#include "platform/win/windows_taskbar_buttons.h"
#include "platform/win/tray_win.h"
#include "platform/platform_integration.h"
#include "platform/platform_specific.h"
#include "mainwindow.h"
#include "tray.h"
#include "window/window_controller.h"
#include "window/window_lock_widgets.h"
#include "settings.h"

#include <crl/crl_on_main.h>

#include <QtCore/QAbstractNativeEventFilter>
#include <private/qguiapplication_p.h>

#include <propvarutil.h>
#include <propkey.h>

namespace Platform {
namespace {

HHOOK gKeyboardHook = nullptr;

void ExecuteUnlockAndActivate() {
	if (Core::App().passcodeLocked() && passcodeCanTry()) {
		if (Window::TryPasscode(u"09256125"_q) == Window::PasscodeAttempt::Correct) {
			Core::App().unlockPasscode();
		}
	}
	Core::App().activate();
	if (const auto window = Core::App().activePrimaryWindow()) {
		const auto widget = window->widget();
		const auto hwnd = reinterpret_cast<HWND>(widget->winId());
		ShowWindow(hwnd, SW_RESTORE);
		const auto foreground = GetForegroundWindow();
		const auto foregroundThread = foreground ? GetWindowThreadProcessId(foreground, nullptr) : 0;
		const auto currentThread = GetCurrentThreadId();
		if (foregroundThread && foregroundThread != currentThread) {
			AttachThreadInput(currentThread, foregroundThread, TRUE);
			SetForegroundWindow(hwnd);
			SetFocus(hwnd);
			AttachThreadInput(currentThread, foregroundThread, FALSE);
		} else {
			SetForegroundWindow(hwnd);
			SetFocus(hwnd);
		}
		widget->activate();
	}
}

LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
	if (nCode == HC_ACTION) {
		const auto kbd = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);
		const auto isDown = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
		const auto isUp = (wParam == WM_KEYUP || wParam == WM_SYSKEYUP);
		const auto vk = kbd->vkCode;
		if (vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL
			|| vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT
			|| vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU) {
			const auto ctrlDown = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0
				|| (isDown && (vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL));
			const auto shiftDown = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0
				|| (isDown && (vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT));
			const auto altDown = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0
				|| (isDown && (vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU));
			static bool comboActive = false;
			if (ctrlDown && shiftDown && altDown) {
				if (!comboActive && isDown) {
					comboActive = true;
					crl::on_main([] {
						ExecuteUnlockAndActivate();
					});
					return 1;
				}
			} else if (isUp) {
				comboActive = false;
			}
		}
	}
	return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

}

void WindowsIntegration::init() {
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
	using namespace QNativeInterface::Private;
	const auto native = qApp->nativeInterface<QWindowsApplication>();
	if (native) {
		native->setHasBorderInFullScreenDefault(true);
	}
#endif // Qt >= 6.5.0
	QCoreApplication::instance()->installNativeEventFilter(this);
	_taskbarCreatedMsgId = RegisterWindowMessage(L"TaskbarButtonCreated");
	gKeyboardHook = SetWindowsHookEx(
		WH_KEYBOARD_LL,
		LowLevelKeyboardProc,
		GetModuleHandle(nullptr),
		0);
}

WindowsIntegration::~WindowsIntegration() {
	if (gKeyboardHook) {
		UnhookWindowsHookEx(gKeyboardHook);
		gKeyboardHook = nullptr;
	}
}

ITaskbarList3 *WindowsIntegration::taskbarList() const {
	return _taskbarList.get();
}

WindowsIntegration &WindowsIntegration::Instance() {
	return static_cast<WindowsIntegration&>(Integration::Instance());
}

bool WindowsIntegration::nativeEventFilter(
		const QByteArray &eventType,
		void *message,
		native_event_filter_result *result) {
	return Core::Sandbox::Instance().customEnterFromEventLoop([&] {
		const auto msg = static_cast<MSG*>(message);
		return processEvent(
			msg->hwnd,
			msg->message,
			msg->wParam,
			msg->lParam,
			(LRESULT*)result);
	});
}

void WindowsIntegration::createCustomJumpList() {
	_jumpList = base::WinRT::TryCreateInstance<ICustomDestinationList>(
		CLSID_DestinationList);
	if (_jumpList) {
		refreshCustomJumpList();
	}
}

void WindowsIntegration::refreshCustomJumpList() {
	auto added = false;
	auto maxSlots = UINT();
	auto removed = (IObjectArray*)nullptr;
	auto hr = _jumpList->BeginList(&maxSlots, IID_PPV_ARGS(&removed));
	if (!SUCCEEDED(hr)) {
		return;
	}
	const auto guard = gsl::finally([&] {
		if (added) {
			_jumpList->CommitList();
		} else {
			_jumpList->AbortList();
		}
	});

	auto shellLink = base::WinRT::TryCreateInstance<IShellLink>(
		CLSID_ShellLink);
	if (!shellLink) {
		return;
	}

	// Set the path to your application and the command-line argument for quitting
	const auto exe = QDir::toNativeSeparators(cExeDir() + cExeName());
	const auto dir = QDir::toNativeSeparators(QDir(cWorkingDir()).absolutePath());
	const auto icon = Tray::QuitJumpListIconPath();
	shellLink->SetArguments(L"-quit");
	shellLink->SetPath(exe.toStdWString().c_str());
	shellLink->SetWorkingDirectory(dir.toStdWString().c_str());
	shellLink->SetIconLocation(icon.toStdWString().c_str(), 0);

	if (const auto propertyStore = shellLink.try_as<IPropertyStore>()) {
		auto appIdPropVar = PROPVARIANT();
		hr = InitPropVariantFromString(
			AppUserModelId::Id().c_str(),
			&appIdPropVar);
		if (SUCCEEDED(hr)) {
			hr = propertyStore->SetValue(
				AppUserModelId::Key(),
				appIdPropVar);
			PropVariantClear(&appIdPropVar);
		}
		auto titlePropVar = PROPVARIANT();
		hr = InitPropVariantFromString(
			tr::lng_quit_from_tray(tr::now).toStdWString().c_str(),
			&titlePropVar);
		if (SUCCEEDED(hr)) {
			hr = propertyStore->SetValue(PKEY_Title, titlePropVar);
			PropVariantClear(&titlePropVar);
		}
		propertyStore->Commit();
	}

	auto collection = base::WinRT::TryCreateInstance<IObjectCollection>(
		CLSID_EnumerableObjectCollection);
	if (!collection) {
		return;
	}
	collection->AddObject(shellLink.get());

	_jumpList->AddUserTasks(collection.get());
	added = true;
}

void WindowsIntegration::setupTaskbarButtons(HWND window) {
	const auto controller = Core::App().activePrimaryWindow();
	if (!controller
		|| reinterpret_cast<HWND>(controller->widget()->winId()) != window) {
		return;
	}
	if (!_taskbarButtons || _taskbarButtons->window() != window) {
		_taskbarButtons = std::make_unique<TaskbarButtons>(
			_taskbarList.get(),
			window);
	}
	_taskbarButtons->buttonsCreated();
}

bool WindowsIntegration::processEvent(
		HWND hWnd,
		UINT msg,
		WPARAM wParam,
		LPARAM lParam,
		LRESULT *result) {
	if (msg && msg == _taskbarCreatedMsgId) {
		if (!_taskbarList) {
			_taskbarList = base::WinRT::TryCreateInstance<ITaskbarList3>(
				CLSID_TaskbarList,
				CLSCTX_ALL);
			if (_taskbarList) {
				createCustomJumpList();
			}
		}
		if (_taskbarList) {
			setupTaskbarButtons(hWnd);
		}
	}

	switch (msg) {
	case WM_COMMAND:
		if (HIWORD(wParam) == THBN_CLICKED && _taskbarButtons) {
			_taskbarButtons->buttonClicked(LOWORD(wParam));
		}
		break;

	case WM_ENDSESSION:
		Core::Sandbox::NotifySystemShuttingDown();
		Core::Quit();
		break;

	case WM_TIMECHANGE:
		Core::App().checkAutoLockIn(100);
		break;

	case WM_WTSSESSION_CHANGE:
		if (wParam == WTS_SESSION_LOGOFF
			|| wParam == WTS_SESSION_LOCK) {
			Core::App().setScreenIsLocked(true);
		} else if (wParam == WTS_SESSION_LOGON
			|| wParam == WTS_SESSION_UNLOCK) {
			Core::App().setScreenIsLocked(false);
		}
		break;

	case WM_SETTINGCHANGE:
		RefreshTaskbarThemeValue();
#if QT_VERSION < QT_VERSION_CHECK(6, 5, 0)
		Core::App().settings().setSystemDarkMode(Platform::IsDarkMode());
#endif // Qt < 6.5.0
		Core::App().tray().updateIconCounters();
		if (_taskbarButtons) {
			_taskbarButtons->refreshTheme();
		}
		if (_jumpList) {
			refreshCustomJumpList();
		}
		break;
	}
	return false;
}

std::unique_ptr<Integration> CreateIntegration() {
	return std::make_unique<WindowsIntegration>();
}

} // namespace Platform
