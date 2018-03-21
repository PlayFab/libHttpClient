﻿#include "pch.h"
#include "MainPage.xaml.h"
#include <httpClient\httpClient.h>

#include <regex>
#include <sstream>
#include <iomanip>
#include <codecvt>

using namespace HttpTestApp;
using namespace Platform;
using namespace Windows::Foundation;
using namespace Windows::Foundation::Collections;
using namespace Windows::UI::Xaml;
using namespace Windows::UI::Xaml::Controls;
using namespace Windows::UI::Xaml::Controls::Primitives;
using namespace Windows::UI::Xaml::Data;
using namespace Windows::UI::Xaml::Input;
using namespace Windows::UI::Xaml::Media;
using namespace Windows::UI::Xaml::Navigation;

static HttpTestApp::MainPage^ g_MainPage;
static Windows::UI::Core::CoreDispatcher^ s_dispatcher;
class win32_handle
{
public:
    win32_handle() : m_handle(nullptr)
    {
    }

    ~win32_handle()
    {
        if (m_handle != nullptr) CloseHandle(m_handle);
        m_handle = nullptr;
    }

    void set(HANDLE handle)
    {
        m_handle = handle;
    }

    HANDLE get() { return m_handle; }

private:
    HANDLE m_handle;
};

win32_handle g_stopRequestedHandle;
win32_handle g_workReadyHandle;
win32_handle g_completionReadyHandle;

#define TICKS_PER_SECOND 10000000i64


static std::string to_utf8string(const std::wstring& input)
{
    std::wstring_convert<std::codecvt_utf8<wchar_t>, wchar_t> utfConverter;
    return utfConverter.to_bytes(input);
}

static std::wstring to_utf16string(const std::string& input)
{
    std::wstring_convert<std::codecvt_utf8<wchar_t>, wchar_t> utfConverter;
    return utfConverter.from_bytes(input);
}

std::string format_string(_Printf_format_string_ char const* format, ...)
{
    char message[4096] = {};

    va_list varArgs = nullptr;
    va_start(varArgs, format);
    vsprintf_s(message, format, varArgs); 
    va_end(varArgs);

    return message;
}

void HandleAsyncQueueCallback(
    _In_ void* context,
    _In_ async_queue_t queue,
    _In_ AsyncQueueCallbackType type
)
{
    UNREFERENCED_PARAMETER(context);
    UNREFERENCED_PARAMETER(queue);

    switch (type)
    {
    case AsyncQueueCallbackType::AsyncQueueCallbackType_Work:
        SetEvent(g_workReadyHandle.get());
        break;

    case AsyncQueueCallbackType::AsyncQueueCallbackType_Completion:
        SetEvent(g_completionReadyHandle.get());
        break;
    }
}

MainPage::MainPage()
{
    s_dispatcher = this->Dispatcher;
    m_websocket = nullptr;
    g_MainPage = this;

    g_stopRequestedHandle.set(CreateEvent(nullptr, true, false, nullptr));
    g_workReadyHandle.set(CreateEvent(nullptr, false, false, nullptr));
    g_completionReadyHandle.set(CreateEvent(nullptr, false, false, nullptr));
    InitializeComponent();

    HCGlobalInitialize();
    HCSettingsSetLogLevel(HC_LOG_LEVEL::LOG_VERBOSE);

    uint32_t sharedAsyncQueueId = 0;
    CreateSharedAsyncQueue(
        sharedAsyncQueueId,
        AsyncQueueDispatchMode::AsyncQueueDispatchMode_Manual,
        AsyncQueueDispatchMode::AsyncQueueDispatchMode_Manual,
        &m_queue);
    AddAsyncCallbackSubmitted(m_queue, nullptr, HandleAsyncQueueCallback, &m_callbackToken);

    StartBackgroundThread();

    TextboxURL->Text = L"wss://rta.xboxlive.com/connect";    
    TextboxHeaders->Text = L"Accept-Language: en-US; Authorization: XBL3.0 x=TBD; Signature: TBD";

    TextboxMethod->Text = L"rta.xboxlive.com.V2";
    TextboxTimeout->Text = L"120";
    TextboxRequestString->Text = L"[1,1,\"http://social.xboxlive.com/users/xuid(2814653827156252)/friends\"]";
}

MainPage::~MainPage()
{
    HCGlobalCleanup();
}

std::vector<std::vector<std::string>> ExtractHeadersFromHeadersString(std::string headersList)
{
    std::vector<std::vector<std::string>> headers;
    std::regex headersListToken("; ");
    std::sregex_token_iterator iterHeadersList(headersList.begin(), headersList.end(), headersListToken, -1);
    std::sregex_token_iterator endHeadersList;
    std::vector<std::string> headerList(iterHeadersList, endHeadersList);
    for (auto header : headerList)
    {
        std::regex headerToken(": ");
        std::sregex_token_iterator iterHeader(header.begin(), header.end(), headerToken, -1);
        std::sregex_token_iterator endHeader;
        std::vector<std::string> valueKeyPair(iterHeader, endHeader);
        if (valueKeyPair.size() == 2)
        {
            headers.push_back(valueKeyPair);
        }
    }

    return headers;
}

DWORD WINAPI background_thread_proc(LPVOID lpParam)
{
    HANDLE hEvents[3] =
    {
        g_workReadyHandle.get(),
        g_completionReadyHandle.get(),
        g_stopRequestedHandle.get()
    };

    async_queue_t queue;
    uint32_t sharedAsyncQueueId = 0;
    CreateSharedAsyncQueue(
        sharedAsyncQueueId,
        AsyncQueueDispatchMode::AsyncQueueDispatchMode_Manual,
        AsyncQueueDispatchMode::AsyncQueueDispatchMode_Manual,
        &queue);

    bool stop = false;
    uint64_t taskGroupId = 0;
    while (!stop)
    {
        DWORD dwResult = WaitForMultipleObjectsEx(3, hEvents, false, INFINITE, false);
        switch (dwResult)
        {
        case WAIT_OBJECT_0: // work ready
            DispatchAsyncQueue(queue, AsyncQueueCallbackType_Work, 0);

            if (!IsAsyncQueueEmpty(queue, AsyncQueueCallbackType_Work))
            {
                // If there's more pending work, then set the event to process them
                SetEvent(g_workReadyHandle.get());
            }
            break;

        case WAIT_OBJECT_0 + 1: // completed 
            // Typically completions should be dispatched on the game thread, but
            // for this simple XAML app we're doing it here
            DispatchAsyncQueue(queue, AsyncQueueCallbackType_Completion, 0);

            if (!IsAsyncQueueEmpty(queue, AsyncQueueCallbackType_Completion))
            {
                // If there's more pending completions, then set the event to process them
                SetEvent(g_completionReadyHandle.get());
            }
            break;
        default:
            stop = true;
            break;
        }
    }

    return 0;
}

void HttpTestApp::MainPage::StopBackgroundThread()
{
    if (m_hBackgroundThread != nullptr)
    {
        SetEvent(g_stopRequestedHandle.get());
        WaitForSingleObject(m_hBackgroundThread, INFINITE);
        CloseHandle(m_hBackgroundThread);
        m_hBackgroundThread = nullptr;
    }
}

void HttpTestApp::MainPage::StartBackgroundThread()
{
    if (m_hBackgroundThread == nullptr)
    {
        m_hBackgroundThread = CreateThread(nullptr, 0, background_thread_proc, nullptr, 0, nullptr);
    }
}

void HttpTestApp::MainPage::ClearLog()
{
    // This callback is called from background thread and we must set the XAML text on the UI thread, so use CoreDispatcher to get it there
    s_dispatcher->RunAsync(Windows::UI::Core::CoreDispatcherPriority::Normal, ref new Windows::UI::Core::DispatchedHandler([]()
    {
        g_MainPage->LogTextBox->Text = ref new Platform::String(L"");
    }));
}

void HttpTestApp::MainPage::LogToUI(std::string str)
{
    // This callback is called from background thread and we must set the XAML text on the UI thread, so use CoreDispatcher to get it there
    s_dispatcher->RunAsync(Windows::UI::Core::CoreDispatcherPriority::Normal, ref new Windows::UI::Core::DispatchedHandler([str]()
    {
        g_MainPage->LogTextBox->Text += ref new Platform::String(to_utf16string(str).c_str());
        g_MainPage->LogTextBox->Text += ref new Platform::String(L"\r\n");
    }));
}

void HttpTestApp::MainPage::Connect_Button_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e)
{
    std::string requestHeaders = to_utf8string(TextboxHeaders->Text->Data());
    std::string requestSubprotocol = to_utf8string(TextboxMethod->Text->Data());
    std::string requestUrl = to_utf8string(TextboxURL->Text->Data());

    ClearLog();

    HC_RESULT hr = HCWebSocketCreate(&m_websocket);
    LogToUI(format_string("HCWebSocketCreate: %d", hr));

    HC_SUBSYSTEM_ID taskSubsystemId = HC_SUBSYSTEM_ID_GAME;
    uint64_t taskGroupId = 0;
    void* callbackContext = nullptr;

    auto headers = ExtractHeadersFromHeadersString(requestHeaders);
    for (auto header : headers)
    {
        std::string headerName = header[0];
        std::string headerValue = header[1];
        hr = HCWebSocketSetHeader(m_websocket, headerName.c_str(), headerValue.c_str());
    }

    AsyncBlock* asyncBlock = new AsyncBlock;
    ZeroMemory(asyncBlock, sizeof(AsyncBlock));
    asyncBlock->queue = m_queue;
    asyncBlock->callback = [](AsyncBlock* asyncBlock)
    {
        WebSocketCompletionResult result = {};
        HCGetWebSocketConnectResult(asyncBlock, &result);

        g_MainPage->LogToUI(format_string("HCWebSocketConnect complete: %d, %d", result.errorCode, result.platformErrorCode));
        delete asyncBlock;
    };

    hr = HCWebSocketConnect(requestUrl.c_str(), requestSubprotocol.c_str(), m_websocket, asyncBlock);
    LogToUI(format_string("HCWebSocketConnect: %d", hr));

}

void HttpTestApp::MainPage::SendMessage_Button_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e)
{
    HC_SUBSYSTEM_ID taskSubsystemId = HC_SUBSYSTEM_ID_GAME;
    uint64_t taskGroupId = 0;
    void* callbackContext = nullptr;

    std::string requestBody = to_utf8string(TextboxRequestString->Text->Data());

    AsyncBlock* asyncBlock = new AsyncBlock;
    ZeroMemory(asyncBlock, sizeof(AsyncBlock));
    asyncBlock->queue = m_queue;
    asyncBlock->callback = [](AsyncBlock* asyncBlock)
    {
        WebSocketCompletionResult result = {};
        HCGetWebSocketConnectResult(asyncBlock, &result);

        g_MainPage->LogToUI(format_string("HCWebSocketSendMessage complete: %d, %d", result.errorCode, result.platformErrorCode));
        delete asyncBlock;
    };

    HC_RESULT hr = HCWebSocketSendMessage(m_websocket, requestBody.c_str(), asyncBlock);
    LogToUI(format_string("HCWebSocketSendMessage: %d", hr));
}

void HttpTestApp::MainPage::Close_Button_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e)
{
    HC_RESULT hr = HCWebSocketDisconnect(m_websocket);
    LogToUI(format_string("HCWebSocketCloseHandle: %d", hr));
    m_websocket = nullptr;
}

