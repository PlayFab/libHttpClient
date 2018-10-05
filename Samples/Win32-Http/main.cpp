// Copyright (c) Microsoft Corporation
// Licensed under the MIT license. See LICENSE file in the project root for full license information.
#include "stdafx.h"
#include "httpClient\httpClient.h"
#include "json_cpp\json.h"
#include "bond_lite\All.hpp"
#include "pal\PAL.hpp"
#include <chrono>
#include "../../bondlite/generated/DataPackage_types.hpp"
//#include "../../bondlite/generated/DataPackage_writers.hpp"
//#include "../../bondlite/generated/DataPackage_readers.hpp"
#include "../../lib/bond/generated/AriaProtocol_types.hpp"
#include "../../lib/bond//generated/AriaProtocol_readers.hpp"
#include "../../lib/bond/generated/AriaProtocol_writers.hpp"

// Not using std::vector<> directly to be able to provide
// custom formatter which dumps the full data.
class FullDumpBinaryBlob : public std::vector<uint8_t>
{
public:
	FullDumpBinaryBlob() {}
	FullDumpBinaryBlob(std::initializer_list<uint8_t> values) : std::vector<uint8_t>(values) {}
}; 
AriaProtocol::Record  source;
FullDumpBinaryBlob liteBondOutput;
void serialize()
{
	/*source.ver = "3.0";
	source.name = "stats";
	source.iKey = "o:ead4d35d9f17486581d6c09afbe41263";
	source.flags = 0;
	source.cV = "";
	source.extProtocol.resize(1);
	source.extProtocol[0].metadataCrc = 0;
	source.extProtocol[0].devMake = "ASUS";
	source.extProtocol[0].devModel = "All Series";

	source.extDevice.resize(1);
	source.extDevice[0].localId = "c:905464f0-7ca9-4f78-8e70-8e5ad384ea1a";
	source.extDevice[0].deviceClass = "Windows.Desktop";

	source.extOs.resize(1);
	source.extOs[0].name = "Windows Desktop";
	source.extOs[0].ver = "10.0.17134.1.amd64fre.rs4_release.180410-1804";

	source.extApp.resize(1);
	source.extApp[0].id = "HelloAria";

	source.extSdk.resize(1);
	source.extSdk[0].libVer = "EVT-Windows-C++-No-3.0.275.1";
	source.extSdk[0].epoch = "7DD8D2EC-BDB5-48C2-8148-4C900FCEE990";
	source.extSdk[0].seq = 1;
	source.extSdk[0].installId = "C34A2D50-CB8A-4330-B249-DDCAB22B2975";

	source.baseType = "act_stats";
	source.data.resize(1);
	
	AriaProtocol::Value val;
	source.data[0].properties["act_stats_id"] = val;
	
	AriaProtocol::Value val1;
	val1.stringValue = "SQLite/Default";
	val1.doubleValue = 0.00000000000000000;
	source.data[0].properties["off_type"] = val1;

	AriaProtocol::Value val2;
	val2.stringValue = "1538593610979";
	val2.doubleValue = 0.00000000000000000;
	source.data[0].properties["s_Firststime"] = val2;

	AriaProtocol::Value val3;
	val3.stringValue = "60";
	source.data[0].properties["st_freq"] = val3;
*/

	{
		bond_lite::CompactBinaryProtocolWriter writer(liteBondOutput);
		bond_lite::Serialize(writer, source);
	}}

void deserialize() {
	AriaProtocol::Record record2;
	{
		bond_lite::CompactBinaryProtocolReader reader(liteBondOutput);
		bond_lite::Deserialize(reader, record2);
	}
}

std::vector<std::vector<std::string>> ExtractAllHeaders(_In_ hc_call_handle_t call)
{
    uint32_t numHeaders = 0;
    HCHttpCallResponseGetNumHeaders(call, &numHeaders);

    std::vector< std::vector<std::string> > headers;
    for (uint32_t i = 0; i < numHeaders; i++)
    {
        const char* str;
        const char* str2;
        std::string headerName;
        std::string headerValue;
        HCHttpCallResponseGetHeaderAtIndex(call, i, &str, &str2);
        if (str != nullptr) headerName = str;
        if (str2 != nullptr) headerValue = str2;
        std::vector<std::string> header;
        header.push_back(headerName);
        header.push_back(headerValue);

        headers.push_back(header);
    }

    return headers;
}

class win32_handle
{
public:
    win32_handle() : m_handle(nullptr)
    {
    }

    ~win32_handle()
    {
        if( m_handle != nullptr ) CloseHandle(m_handle);
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
win32_handle g_exampleTaskDone;

DWORD g_targetNumThreads = 2;
HANDLE g_hActiveThreads[10] = { 0 };
DWORD g_defaultIdealProcessor = 0;
DWORD g_numActiveThreads = 0;

async_queue_handle_t g_queue;
registration_token_t g_callbackToken;

DWORD WINAPI background_thread_proc(LPVOID lpParam)
{
    HANDLE hEvents[3] =
    {
        g_workReadyHandle.get(),
        g_completionReadyHandle.get(),
        g_stopRequestedHandle.get()
    };

    async_queue_handle_t queue;
    uint32_t sharedAsyncQueueId = 0;
    CreateSharedAsyncQueue(
        sharedAsyncQueueId,
        AsyncQueueDispatchMode::AsyncQueueDispatchMode_Manual,
        AsyncQueueDispatchMode::AsyncQueueDispatchMode_Manual,
        &queue);

    bool stop = false;
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

void CALLBACK HandleAsyncQueueCallback(
    _In_ void* context,
    _In_ async_queue_handle_t queue,
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

void StartBackgroundThread()
{
    g_stopRequestedHandle.set(CreateEvent(nullptr, true, false, nullptr));
    g_workReadyHandle.set(CreateEvent(nullptr, false, false, nullptr));
    g_completionReadyHandle.set(CreateEvent(nullptr, false, false, nullptr));
    g_exampleTaskDone.set(CreateEvent(nullptr, false, false, nullptr));

    for (uint32_t i = 0; i < g_targetNumThreads; i++)
    {
        g_hActiveThreads[i] = CreateThread(nullptr, 0, background_thread_proc, nullptr, 0, nullptr);
        if (g_defaultIdealProcessor != MAXIMUM_PROCESSORS)
        {
            if (g_hActiveThreads[i] != nullptr)
            {
                SetThreadIdealProcessor(g_hActiveThreads[i], g_defaultIdealProcessor);
            }
        }
    }

    g_numActiveThreads = g_targetNumThreads;
}

void ShutdownActiveThreads()
{
    SetEvent(g_stopRequestedHandle.get());
    DWORD dwResult = WaitForMultipleObjectsEx(g_numActiveThreads, g_hActiveThreads, true, INFINITE, false);
    if (dwResult >= WAIT_OBJECT_0 && dwResult <= WAIT_OBJECT_0 + g_numActiveThreads - 1)
    {
        for (DWORD i = 0; i < g_numActiveThreads; i++)
        {
            CloseHandle(g_hActiveThreads[i]);
            g_hActiveThreads[i] = nullptr;
        }
        g_numActiveThreads = 0;
        ResetEvent(g_stopRequestedHandle.get());
    }
}

using namespace std;

#include "LogManager.hpp"

enum EventLatency
{
	/// Unspecified: Event Latency is not specified
	EventLatency_Unspecified = -1,

	/// Off: Latency is not to be transmitted
	EventLatency_Off = 0,

	/// Normal: Latency is to be transmitted at low priority
	EventLatency_Normal = 1,

	/// Cost Deffered: Latency is to be transmitted at cost deferred priority
	EventLatency_CostDeferred = 2,

	/// RealTime: Latency is to be transmitted at real time priority
	EventLatency_RealTime = 3,

	/// Max: Latency is to be transmitted as soon as possible
	EventLatency_Max = 4,
};

enum EventPersistence
{
	/// Normal
	EventPersistence_Normal = 1,

	/// Critical
	EventPersistence_Critical = 2,
};

struct EventProperties {
	std::string     m_eventNameP;
	std::string     m_eventTypeP;
	EventLatency     m_eventLatency;
	EventPersistence m_eventPersistence;
	double           m_eventPopSample;
	uint64_t         m_eventPolicyBitflags;
	int64_t          m_timestampInMillis;

	EventProperties(std::string s)
	:m_eventNameP(s)
		,m_eventPopSample(100.0)
	{}
};

uint64_t m_sequenceId = 0;
std::string m_initId = "B8F9A36D-3677-42FF-BE0E-73D3A97A2E74";

bool decorate(::AriaProtocol::Record& record, EventLatency& latency, EventProperties const& eventProperties)
{

	//////Base /////
	if (record.extSdk.size() == 0)
	{
		::AriaProtocol::Sdk sdk;
		record.extSdk.push_back(sdk);
	}

	record.time = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
	record.ver = "3.0";
	if (record.baseType.empty())
	{
		record.baseType = record.name;
	}

	record.extSdk[0].seq = ++m_sequenceId;
	record.extSdk[0].epoch = m_initId;
	std::string sdkVersion = "EVT-Windows-C++-No-3.0.275.1";
	record.extSdk[0].libVer = sdkVersion;
	record.extSdk[0].installId = "C34A2D50 - CB8A - 4330 - B249 - DDCAB22B2975";// m_owner.GetLogSessionData()->getSessionSDKUid();

	//////Semantic context//////

	if (record.data.size() == 0)
	{
		::AriaProtocol::Data data;
		record.data.push_back(data);
	}
	if (record.extApp.size() == 0)
	{
		::AriaProtocol::App app;
		record.extApp.push_back(app);
	}
	if (record.extDevice.size() == 0)
	{
		::AriaProtocol::Device device;
		record.extDevice.push_back(device);
	}

	if (record.extOs.size() == 0)
	{
		::AriaProtocol::Os os;
		record.extOs.push_back(os);
	}

	if (record.extUser.size() == 0)
	{
		::AriaProtocol::User user;
		record.extUser.push_back(user);
	}

	if (record.extLoc.size() == 0)
	{
		::AriaProtocol::Loc loc;
		record.extLoc.push_back(loc);
	}

	if (record.extNet.size() == 0)
	{
		::AriaProtocol::Net net;
		record.extNet.push_back(net);
	}

	if (record.extProtocol.size() == 0)
	{
		::AriaProtocol::Protocol proto;
		record.extProtocol.push_back(proto);
	}
	record.extApp[0].id = "Win32Http";// "HelloAria";
	record.extApp[0].ver = "";
	record.extApp[0].locale = "";
	record.extDevice[0].localId = "c:905464f0-7ca9-4f78-8e70-8e5ad384ea1a";
	record.extProtocol[0].devMake = "ASUS";
	record.extProtocol[0].devModel = "All Series";
	record.extDevice[0].deviceClass = "Windows.Desktop";
	record.extOs[0].ver = "10.0.17134.1.amd64fre.rs4_release.180410-1804";
	record.extUser[0].localId = "";
	record.extUser[0].locale = "";
	record.extLoc[0].timezone = "-07:00";
	record.extDevice[0].authSecId = "";
	record.extNet[0].cost = "Unmetered";
	record.extNet[0].provider = "";
	record.extNet[0].type = "Unknown";

     /////////Event properties///////
	if(latency == EventLatency_Unspecified)
		latency = EventLatency_Normal;
	if (record.data.size() == 0)
	{
		::AriaProtocol::Data data;
		record.data.push_back(data);
	}

	record.popSample = eventProperties.m_eventPopSample;

	int64_t flags = 0;
	if (EventPersistence_Critical == eventProperties.m_eventPersistence)
	{
		flags = flags | 0x02;
	}
	else
	{
		flags = flags | 0x01;
	}

	if (latency >= EventLatency_RealTime)
	{
		flags = flags | 0x0200;
	}
	else if (latency == EventLatency_CostDeferred)
	{
		flags = flags | 0x0300;
	}
	else
	{
		flags = flags | 0x0100;
	}
	record.flags = flags;

	return true;

}
bool applyCommonDecorators(::AriaProtocol::Record& record, EventProperties const& properties, EventLatency& latency)
{
	record.ver = "3.0";
	record.name = properties.m_eventNameP;
	
	record.baseType = "custom";
	
	record.iKey = "o:6d084bbf6a9644ef83f40a77c9e34580";
	bool result = true;
	result &= decorate(record, latency, properties);
	return result;
}

void LogEvent(EventProperties const& properties, ::AriaProtocol::Record& record)
{
	EventLatency latency = EventLatency_Normal;
	if (properties.m_eventLatency > EventLatency_Unspecified)
	{
		latency = properties.m_eventLatency;
	}

	applyCommonDecorators(record, properties, latency);
}

int main()
{
	
    std::string method = "Post";
       
	std::string url = "https://self.events.data.microsoft.com/OneCollector/1.0/";// "https://mobile.pipe.aria.microsoft.com/Collector/3.0"; //"https://raw.githubusercontent.com/Microsoft/libHttpClient/master/Samples/Win32-Http/TestContent.json";

	::AriaProtocol::Record record;
																				 
	EventProperties event("TestEvent2");
	LogEvent(event, record);

	std::vector<uint8_t> buffer;
	{
		bond_lite::CompactBinaryProtocolWriter writer(buffer);
		bond_lite::Serialize(writer, record);
	}

//	std::ifstream file("C:\\Users\\v-annme\\Desktop\\ariasample.bin", std::ifstream::in | std::ifstream::binary);
	//std::ifstream file("C:\\Users\\v-annme\\Desktop\\OneDrive.bond.bin", std::ifstream::in | std::ifstream::binary);

	//std::ifstream file("C:\sandbox\proto\Aria.SDK.Cpp\examples\HelloAria\BODYData.txt", std::ifstream::in | std::ifstream::binary);


/*	std::vector<uint8_t> buffer((
		std::istreambuf_iterator<char>(file)),
		(std::istreambuf_iterator<char>()));
	AriaProtocol::Record  source1;
	{
		bond_lite::CompactBinaryProtocolReader reader(buffer);
		bond_lite::Deserialize(reader, source1);
	}
	{
		bond_lite::CompactBinaryProtocolWriter writer(buffer);
		bond_lite::Serialize(writer, source1);
	}
	*/
    bool retryAllowed = true;
    std::vector<std::vector<std::string>> headers;
    std::vector< std::string > header;
	
	header.clear();
	header.push_back("Accept");
	header.push_back("*/*");
	headers.push_back(header);

	header.clear();
	header.push_back("APIKey");
	//header.push_back("x-apikey");
	//header.push_back("194626ba46434f9ab441dd7ebda2aa64-5f64bebb-ac28-4cc7-bd52-570c8fe077c9-7717");
	header.push_back("6d084bbf6a9644ef83f40a77c9e34580-c2d379e0-4408-4325-9b4d-2a7d78131e14-7322");
	headers.push_back(header);

	header.clear();
	header.push_back("Client-Id");
	header.push_back("NO_AUTH");
	headers.push_back(header);


	header.clear();
	header.push_back("Content-Type");
	header.push_back("application/bond-compact-binary");
	headers.push_back(header);

	header.clear();
	header.push_back("Expect");
	header.push_back("100-continue");
	headers.push_back(header);

	header.clear();
	header.push_back("SDK-Version");
	//using namespace  ARIASDK_NS_BEGIN::PAL;
	header.push_back("EVT-Windows-C++-No-3.0.275.1");
	//header.push_back("ACT-Windows Desktop-C++-no-1.7.306.1-no");/// PAL::getSdkVersion());
	headers.push_back(header);
	
	header.clear();
	header.push_back("Upload-Time");
	__int64 now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
	header.push_back(std::to_string(now));
	headers.push_back(header);

	header.clear();
	header.push_back("Content-Length");
	/*std::string str(requestBody);*/
	/*header.push_back(std::to_string(str.size()));*/
	header.push_back(std::to_string(buffer.size()));
	headers.push_back(header);

	header.clear();
	header.push_back("Connection");
	header.push_back("Keep-Alive");
	headers.push_back(header);

	header.clear();
	header.push_back("Cache-Control");
	header.push_back("no-cache");
	headers.push_back(header);

	header.clear();
	


    HCInitialize(nullptr);

    uint32_t sharedAsyncQueueId = 0;
    CreateSharedAsyncQueue(
        sharedAsyncQueueId,
        AsyncQueueDispatchMode::AsyncQueueDispatchMode_Manual,
        AsyncQueueDispatchMode::AsyncQueueDispatchMode_Manual,
        &g_queue);
    RegisterAsyncQueueCallbackSubmitted(g_queue, nullptr, HandleAsyncQueueCallback, &g_callbackToken);

    StartBackgroundThread();

    hc_call_handle_t call = nullptr;
    HCHttpCallCreate(&call);
    HCHttpCallRequestSetUrl(call, method.c_str(), url.c_str());
	HCHttpCallRequestSetRequestBodyBytes(call, buffer.data(), buffer.size());
    HCHttpCallRequestSetRetryAllowed(call, retryAllowed);
    for (auto& header : headers)
    {
        std::string headerName = header[0];
        std::string headerValue = header[1];
        HCHttpCallRequestSetHeader(call, headerName.c_str(), headerValue.c_str(), true);
    }

    printf_s("Calling %s %s\r\n", method.c_str(), url.c_str());

    AsyncBlock* asyncBlock = new AsyncBlock;
    ZeroMemory(asyncBlock, sizeof(AsyncBlock));
    asyncBlock->context = call;
    asyncBlock->queue = g_queue;
    asyncBlock->callback = [](AsyncBlock* asyncBlock)
    {
        const char* str;
        HRESULT errCode = S_OK;
        uint32_t platErrCode = 0;
        uint32_t statusCode = 0;
        std::string responseString;
        std::string errMessage;

        hc_call_handle_t call = static_cast<hc_call_handle_t>(asyncBlock->context);
        HCHttpCallResponseGetNetworkErrorCode(call, &errCode, &platErrCode);
        HCHttpCallResponseGetStatusCode(call, &statusCode);
        HCHttpCallResponseGetResponseString(call, &str);
        if (str != nullptr) responseString = str;
        std::vector<std::vector<std::string>> headers = ExtractAllHeaders(call);

        // Uncomment to write binary file to disk
        //size_t bufferSize = 0;
        //HCHttpCallResponseGetResponseBodyBytesSize(call, &bufferSize);
        //uint8_t* buffer = new uint8_t[bufferSize];
        //size_t bufferUsed = 0;
        //HCHttpCallResponseGetResponseBodyBytes(call, bufferSize, buffer, &bufferUsed);
        //HANDLE hFile = CreateFile(L"c:\\test\\test.zip", GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
        //DWORD bufferWritten = 0;
        //WriteFile(hFile, buffer, (DWORD)bufferUsed, &bufferWritten, NULL);
        //CloseHandle(hFile);
        //delete[] buffer;

        HCHttpCallCloseHandle(call);

        printf_s("HTTP call done\r\n");
        printf_s("Network error code: %d\r\n", errCode);
        printf_s("HTTP status code: %d\r\n", statusCode);

        int i = 0;
        for (auto& header : headers)
        {
            printf_s("Header[%d] '%s'='%s'\r\n", i, header[0].c_str(), header[1].c_str());
            i++;
        }

        if (responseString.length() > 0)
        {
            // Returned string starts with a BOM strip it out.
            uint8_t BOM[] = { 0xef, 0xbb, 0xbf, 0x0 };
            if (responseString.find(reinterpret_cast<char*>(BOM)) == 0)
            {
                responseString = responseString.substr(3);
            }
            web::json::value json = web::json::value::parse(utility::conversions::to_string_t(responseString));;
        }

        if (responseString.length() > 200)
        {
            std::string subResponseString = responseString.substr(0, 200);
            printf_s("Response string:\r\n%s...\r\n", subResponseString.c_str());
        }
        else
        {
            printf_s("Response string:\r\n%s\r\n", responseString.c_str());
        }

        SetEvent(g_exampleTaskDone.get());
        delete asyncBlock;
    };

    HCHttpCallPerformAsync(call, asyncBlock);

    WaitForSingleObject(g_exampleTaskDone.get(), INFINITE);

    ShutdownActiveThreads();
    HCCleanup();

    return 0;
}

