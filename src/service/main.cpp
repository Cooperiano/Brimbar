#include <Windows.h>
#include <UserEnv.h>
#include <WtsApi32.h>

#include <filesystem>
#include <string>

namespace {

constexpr wchar_t service_name[]{L"BrimbarService"};

SERVICE_STATUS_HANDLE status_handle{};
SERVICE_STATUS service_status{};
HANDLE stop_event{};
HANDLE child_job{};

void report_status(const DWORD state, const DWORD error = NO_ERROR, const DWORD wait_hint = 0U) noexcept
{
    service_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    service_status.dwCurrentState = state;
    service_status.dwWin32ExitCode = error;
    service_status.dwWaitHint = wait_hint;
    service_status.dwControlsAccepted = state == SERVICE_RUNNING
        ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN | SERVICE_ACCEPT_SESSIONCHANGE
        : 0U;
    SetServiceStatus(status_handle, &service_status);
}

[[nodiscard]] std::filesystem::path topbar_path()
{
    std::wstring module_path(32768U, L'\0');
    const auto length = GetModuleFileNameW(nullptr, module_path.data(), static_cast<DWORD>(module_path.size()));
    module_path.resize(length);
    return std::filesystem::path(module_path).parent_path() / L"brimbar_topbar.exe";
}

[[nodiscard]] bool launch_for_session(const DWORD session_id) noexcept
{
    HANDLE impersonation_token{};
    if (!WTSQueryUserToken(session_id, &impersonation_token)) {
        return false;
    }

    HANDLE primary_token{};
    const auto duplicated = DuplicateTokenEx(
        impersonation_token,
        TOKEN_ALL_ACCESS,
        nullptr,
        SecurityImpersonation,
        TokenPrimary,
        &primary_token);
    CloseHandle(impersonation_token);
    if (!duplicated) {
        return false;
    }

    void* environment{};
    if (!CreateEnvironmentBlock(&environment, primary_token, FALSE)) {
        CloseHandle(primary_token);
        return false;
    }

    const auto executable = topbar_path();
    const auto command_line_text = L"\"" + executable.wstring() + L"\"";
    std::wstring command_line{command_line_text};
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.lpDesktop = const_cast<wchar_t*>(L"winsta0\\default");
    PROCESS_INFORMATION process{};

    const auto created = CreateProcessAsUserW(
        primary_token,
        executable.c_str(),
        command_line.data(),
        nullptr,
        nullptr,
        FALSE,
        CREATE_UNICODE_ENVIRONMENT,
        environment,
        executable.parent_path().c_str(),
        &startup,
        &process);

    DestroyEnvironmentBlock(environment);
    CloseHandle(primary_token);
    if (!created) {
        return false;
    }

    if (child_job != nullptr) {
        AssignProcessToJobObject(child_job, process.hProcess);
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}

DWORD WINAPI control_handler(const DWORD control, const DWORD event_type, void* event_data, void*) noexcept
{
    if (control == SERVICE_CONTROL_STOP || control == SERVICE_CONTROL_SHUTDOWN) {
        report_status(SERVICE_STOP_PENDING, NO_ERROR, 3000U);
        SetEvent(stop_event);
        return NO_ERROR;
    }
    if (control == SERVICE_CONTROL_SESSIONCHANGE &&
        (event_type == WTS_SESSION_LOGON || event_type == WTS_SESSION_UNLOCK ||
         event_type == WTS_CONSOLE_CONNECT)) {
        const auto notification = static_cast<WTSSESSION_NOTIFICATION*>(event_data);
        if (notification != nullptr) {
            static_cast<void>(launch_for_session(notification->dwSessionId));
        }
    }
    return NO_ERROR;
}

void WINAPI service_main(DWORD, wchar_t**) noexcept
{
    status_handle = RegisterServiceCtrlHandlerExW(service_name, control_handler, nullptr);
    if (status_handle == nullptr) {
        return;
    }

    report_status(SERVICE_START_PENDING, NO_ERROR, 3000U);
    stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    child_job = CreateJobObjectW(nullptr, nullptr);
    if (stop_event == nullptr || child_job == nullptr) {
        report_status(SERVICE_STOPPED, GetLastError());
        return;
    }

    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(child_job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));

    const auto session_id = WTSGetActiveConsoleSessionId();
    if (session_id != 0xFFFFFFFFU) {
        static_cast<void>(launch_for_session(session_id));
    }

    report_status(SERVICE_RUNNING);
    WaitForSingleObject(stop_event, INFINITE);
    CloseHandle(child_job);
    child_job = nullptr;
    CloseHandle(stop_event);
    stop_event = nullptr;
    report_status(SERVICE_STOPPED);
}

}  // namespace

int wmain()
{
    SERVICE_TABLE_ENTRYW dispatch_table[] = {
        {const_cast<wchar_t*>(service_name), service_main},
        {nullptr, nullptr},
    };

    if (!StartServiceCtrlDispatcherW(dispatch_table) && GetLastError() != ERROR_FAILED_SERVICE_CONTROLLER_CONNECT) {
        return 1;
    }
    return 0;
}
