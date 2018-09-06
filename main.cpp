#include <string>
#include <unistd.h>
#include <boost/process.hpp>
#include <boost/log/core.hpp>
#include <boost/log/trivial.hpp>
#include <boost/log/expressions.hpp>
#include <boost/log/utility/setup/file.hpp>
#include <boost/log/sinks/text_file_backend.hpp>
#include <boost/log/utility/setup/common_attributes.hpp>
#include <boost/log/sources/severity_logger.hpp>
#include <boost/log/sources/record_ostream.hpp>
#include <boost/log/support/date_time.hpp>
#include <boost/utility/string_view.hpp>
#include <boost/thread/thread.hpp>
#include <boost/smart_ptr.hpp>
#include <nlohmann/json.hpp>
#include <boost/chrono.hpp>
#include <sys/stat.h>
#include <shellapi.h>
#include <windows.h>
#include <vector>
#include <string>

using json = nlohmann::json;
namespace ch = boost::chrono;
namespace logging = boost::log;
namespace expr = boost::log::expressions;
namespace ps = boost::process;

static HWND hmessage {};
static std::size_t i = 0;
static NOTIFYICONDATA notify {};
static const int WM_TRAYICON = WM_USER + 1;
static ch::steady_clock::time_point process_start;

struct config {
	std::string log;
	std::string runable;
	std::vector<int> rtcode;
};

inline bool mkdirs(const char* dir, uint32_t length) {
	if (!length) length = strlen(dir);
	if (length > MAX_PATH) return false;
	char strbuffer[MAX_PATH] = {};
	for (uint32_t i = 0; i < length && (strbuffer[i] = dir[i]); ++i) {
		if (strbuffer[i] == '\\' && access(strbuffer, F_OK) != 0) {
			if (mkdir(strbuffer) != 0) return false;
		}
	}
	return true;
}

LRESULT CALLBACK tray(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
	switch (message) {
		case WM_TRAYICON:
			switch (lParam) {
				case WM_LBUTTONUP: {
					auto process_duration = ch::duration_cast<ch::minutes>(ch::steady_clock::now() - process_start);
					sprintf(notify.szInfo, "已经守护 %d 次, 当前存活 %d 分钟", i-1, process_duration.count());
					strcpy(notify.szInfoTitle, "提示");
					notify.uTimeout = 2000;
					notify.uFlags = NIF_INFO;
					notify.dwInfoFlags= NIIF_INFO;
					notify.cbSize = sizeof(NOTIFYICONDATA);
					Shell_NotifyIcon(NIM_MODIFY, &notify);
					return TRUE;
				}
				break;
				case WM_RBUTTONUP: {
					POINT pt;
					GetCursorPos(&pt);
					UINT uFlags = TPM_RIGHTBUTTON;
					HMENU menu = CreatePopupMenu();
					AppendMenu(menu, MF_STRING , 0, "退出守护");
					uFlags |= GetSystemMetrics(SM_MENUDROPALIGNMENT)?TPM_RIGHTALIGN:TPM_LEFTALIGN;
					SetForegroundWindow(hmessage);
					TrackPopupMenuEx(menu, uFlags, pt.x, pt.y, hwnd, nullptr);
					DestroyMenu(menu);
					return TRUE;
				}
				break;
			}
			break;
		case WM_COMMAND:
			if (wParam == 0) {
				PostQuitMessage(0);
				return TRUE;
			}
	}
	return DefWindowProc(hwnd, message, wParam, lParam);
}

void protect(boost::shared_ptr<struct config> setting) {
	auto ls = logging::add_file_log(
	              logging::keywords::file_name = setting->log,
	              logging::keywords::time_based_rotation = logging::sinks::file::rotation_at_time_point(0, 0, 0),
	              logging::keywords::format = (
	                          expr::stream
	                          << expr::format_date_time<boost::posix_time::ptime>("TimeStamp", "【%Y-%m-%d %H:%M:%S】")
	                          << " [" << logging::trivial::severity
	                          << "] " << expr::smessage
	                      )
	          );

	if (!ls) MessageBox(nullptr, "创建日志失败！", "错误", MB_OK|MB_ICONERROR);

	bool shouldQuit = false;
	auto thread = boost::thread([&shouldQuit, setting] {
		WNDCLASSEX wx{};
		wx.lpfnWndProc = tray;
		wx.lpszClassName = "super deamon";
		wx.cbSize = sizeof(WNDCLASSEX);
		RegisterClassEx(&wx);
		notify.hIcon = LoadIcon(GetModuleHandle(nullptr), "A");
		notify.hWnd = hmessage = CreateWindowEx(0, wx.lpszClassName, "tray", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
		notify.uFlags = NIF_ICON|NIF_MESSAGE|NIF_TIP;
		notify.uCallbackMessage = WM_TRAYICON;
		strcpy(notify.szTip, "守护天使 [");
		{
			const int MAX_LENGTH = 32;
			const char* target = setting->runable.data();
			const char* sep = strrchr(target, '\\');
			if (!sep) sep = strrchr(target, '/');
			target = sep ? sep + 1 : target;
			auto length = strlen(target);
			strncpy(notify.szTip+10, sep ? sep + 1 : target, MAX_LENGTH);
			strcat(notify.szTip, "...]"+(length > MAX_LENGTH?0:3));
		}
		Shell_NotifyIcon(NIM_ADD, &notify);
		for (MSG msg; GetMessage(&msg, hmessage, 0, 0) > 0; DispatchMessage(&msg)) TranslateMessage(&msg);
		if (!shouldQuit) Shell_NotifyIcon(NIM_DELETE, &notify);
		UnregisterClass(wx.lpszClassName, GetModuleHandle(nullptr));
		shouldQuit = true;
	});

	logging::add_common_attributes();
	logging::sources::severity_logger<logging::trivial::severity_level> lg;

	std::error_code error_code;
	auto rtcodes = setting->rtcode;
	while (!shouldQuit) {
		BOOST_LOG_SEV(lg, logging::trivial::info) << "尝试启动（第" << ++i << "次）";
		if (ls) ls->flush();
		process_start = ch::steady_clock::now();
		auto child = ps::child(setting->runable);
		child.wait(error_code);
		auto process_duration = ch::duration_cast<ch::minutes>(ch::steady_clock::now() - process_start);
		int rtcode = child.exit_code();
		BOOST_LOG_SEV(lg, logging::trivial::info) << "进程已退出，代码："
		        << rtcode << "，存活时间：" << process_duration.count() << "（分钟）" << std::endl;

		if (ls) ls->flush();
		if (std::find(rtcodes.begin(), rtcodes.end(), rtcode) != rtcodes.end()) break;
	}
	if (!shouldQuit) {
		SendMessage(hmessage, WM_COMMAND, 0, 0);
		while (!shouldQuit) Sleep(10);
	}
}

int main(int argc, char* argv[]) {
	char buffer[MAX_PATH] {};
	const char* self = argv[0];
	const char* sep = strrchr(self, '\\');
	if (sep != nullptr) {
		size_t length = sep - self + 1;
		memcpy(buffer, self, length);
		boost::shared_ptr<struct config> setting = nullptr;
		strcpy(buffer+length, "process.config");
		if (!access(buffer, 0)) {
			struct _stat info;
			bool error = false;
			_stat(buffer, &info);
			if (info.st_size < MAX_PATH * 3) {
				FILE * fconfig = fopen(buffer, "rb");
				if (fconfig) {
					auto array = boost::shared_array<char>(new char[info.st_size]);
					for (int aready_rd = 0; aready_rd != info.st_size; ) {
						aready_rd += fread(array.get()+aready_rd, sizeof(char), info.st_size-aready_rd, fconfig);
					}
					fclose(fconfig);
					setting = boost::make_shared<struct config>();
					try {
						auto js = json::parse(boost::string_view(array.get(), info.st_size));
						if (auto logptr = js.find("log"); logptr != js.end()) setting->log = *logptr;
						if (auto runableptr = js.find("target"); runableptr != js.end()) setting->runable = *runableptr;
						if (auto rtcodeptr = js.find("code"); rtcodeptr != js.end()) {
							if (rtcodeptr->is_number()) {
								setting->rtcode.push_back(rtcodeptr->get<int>());
							} else if (rtcodeptr->is_array()) {
								setting->rtcode.reserve(rtcodeptr->size());
								for (auto it = rtcodeptr->begin(); it != rtcodeptr->end(); ++ it) {
									setting->rtcode.push_back(it->get<int>());
								}
							} else {
								error = true;
							}
						}
					} catch(const std::exception& e) {
						error = true;
					}
				}
			} else {
				error = true;
			}
			if (error) {
				MessageBox(nullptr, "不合法的配置文件！", "错误", MB_OK|MB_ICONERROR);
				return 0;
			}
		} else {
			if (sep[1] == 'p' && stricmp(sep+1, "process.exe")) {
				strcpy(buffer+length, sep + 2);
				setting = boost::make_shared<struct config>();
				setting->runable = buffer;
			}
		}
		if (setting) {
			if (setting->log.empty()) setting->log = "process.log";
			if (setting->rtcode.empty()) setting->rtcode.push_back(0);
			if (!setting->runable.empty()) {
				if (access(setting->runable.data(), 0)) {
					MessageBox(nullptr, "目标文件不存在！", "错误", MB_OK|MB_ICONERROR);
				} else {
					bool yes = true;
					if (!mkdirs(setting->log.data(), setting->log.length())) {
						yes = MessageBox(nullptr, "无法创建日志文件夹，是否继续？", "错误", MB_YESNO|MB_ICONERROR) == IDYES;
					}
					if (yes) protect(setting);
				}
			} else {
				MessageBox(nullptr, "未配置目标文件！", "错误", MB_OK|MB_ICONERROR);
			}
		} else {
			MessageBox(nullptr, "未找到配置文件！", "错误", MB_OK|MB_ICONERROR);
		}
	}
	return 0;
}
