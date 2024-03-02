// Copyright (c) Microsoft Corporation
// Licensed under the MIT license. See LICENSE file in the project root for full license information.
#include "pch.h"
#if !HC_XDK_API
#include <Shlwapi.h>
#endif
#include "../httpcall.h"
#include "xmlhttp_http_task.h"
#include "utils.h"
#include "http_request_callback.h"
#include "http_response_stream.h"
#include "http_request_stream.h"

xmlhttp_http_task::xmlhttp_http_task(
    _Inout_ XAsyncBlock* asyncBlock,
    _In_ HCCallHandle call
    ) :
    m_statusCode(0),
    m_call(call),
    m_asyncBlock(asyncBlock)
{
    m_hrCoInit = CoInitializeEx(nullptr, 0);
}

xmlhttp_http_task::~xmlhttp_http_task()
{
    m_hRequest = nullptr;
    if (SUCCEEDED(m_hrCoInit))
    {
        CoUninitialize();
    }
}

void CALLBACK xmlhttp_http_task::PerformAsyncHandler(
    HCCallHandle call,
    XAsyncBlock* asyncBlock,
    void* context,
    HCPerformEnv env
) noexcept
{
    assert(context == nullptr);
    assert(env == nullptr);
    UNREFERENCED_PARAMETER(context);
    UNREFERENCED_PARAMETER(env);

    auto httpTask = http_allocate_shared<xmlhttp_http_task>(asyncBlock, call);
    httpTask->perform_async(asyncBlock, call);
}

void xmlhttp_http_task::perform_async(
    _Inout_ XAsyncBlock* asyncBlock,
    _In_ HCCallHandle call
    )
{
    try
    {
        const char* url = nullptr;
        const char* method = nullptr;
        HCHttpCallRequestBodyReadFunction requestBodyReadFunction = nullptr;
        size_t requestBodySize = 0;
        void* context = nullptr;
        HCHttpCallRequestGetUrl(call, &method, &url);
        HCHttpCallRequestGetRequestBodyReadFunction(call, &requestBodyReadFunction, &requestBodySize, &context);

        uint32_t numHeaders = 0;
        HCHttpCallRequestGetNumHeaders(call, &numHeaders);

        uint32_t timeoutInSeconds = 0;
        HCHttpCallRequestGetTimeout(call, &timeoutInSeconds);

        HRESULT hr = CoCreateInstance(
            __uuidof(FreeThreadedXMLHTTP60),
            nullptr,
#if HC_XDK_API
            CLSCTX_SERVER,
#else
            CLSCTX_INPROC,
#endif
            __uuidof(IXMLHTTPRequest2),
            reinterpret_cast<void**>(m_hRequest.GetAddressOf()));
        if (FAILED(hr))
        {
            HC_TRACE_ERROR(HTTPCLIENT, "Failure to create IXMLHTTPRequest2 instance 0x%0.8x", hr);
            HCHttpCallResponseSetNetworkErrorCode(call, hr, hr);
            XAsyncComplete(asyncBlock, S_OK, 0);
            return;
        }

        std::shared_ptr<hc_task> httpTask2 = shared_from_this();
        std::shared_ptr<xmlhttp_http_task> httpTask = std::dynamic_pointer_cast<xmlhttp_http_task>(httpTask2);

        http_internal_wstring wMethod = utf16_from_utf8(method);
        http_internal_wstring wUrl = utf16_from_utf8(url);
        hr = m_hRequest->Open(
            wMethod.c_str(),
            wUrl.c_str(),
            Microsoft::WRL::Make<http_request_callback>(httpTask).Get(),
            nullptr,
            nullptr,
            nullptr,
            nullptr);
        if (FAILED(hr))
        {
            HC_TRACE_ERROR(HTTPCLIENT, "Failure to open HTTP request 0x%0.8x", hr);
            HCHttpCallResponseSetNetworkErrorCode(call, hr, hr);
            XAsyncComplete(asyncBlock, S_OK, 0);
            return;
        }

        m_hRequest->SetProperty(XHR_PROP_NO_CRED_PROMPT, TRUE);

        ULONGLONG timeout = static_cast<ULONGLONG>(timeoutInSeconds * 1000);
        m_hRequest->SetProperty(XHR_PROP_TIMEOUT, timeout);

#ifdef XHR_PROP_ONDATA_NEVER
        m_hRequest->SetProperty(XHR_PROP_ONDATA_THRESHOLD, XHR_PROP_ONDATA_NEVER);
#endif

        bool userAgentSet = false;
        for (uint32_t i = 0; i < numHeaders; i++)
        {
            const char* iHeaderName;
            const char* iHeaderValue;
            HCHttpCallRequestGetHeaderAtIndex(call, i, &iHeaderName, &iHeaderValue);
            if (iHeaderName != nullptr && iHeaderValue != nullptr)
            {
                if (xbox::httpclient::str_icmp(iHeaderName, "User-Agent") == 0)
                {
                    userAgentSet = true;
                }

                hr = m_hRequest->SetRequestHeader(utf16_from_utf8(iHeaderName).c_str(), utf16_from_utf8(iHeaderValue).c_str());
            }
        }

        if (!userAgentSet)
        {
            m_hRequest->SetRequestHeader(L"User-Agent", L"libHttpClient/1.0.0.0");
        }

        hr = m_hRequest->SetCustomResponseStream(Microsoft::WRL::Make<http_response_stream>(httpTask).Get());
        if (FAILED(hr))
        {
            HC_TRACE_ERROR(HTTPCLIENT, "Failure to set HTTP response stream 0x%0.8x", hr);
            HCHttpCallResponseSetNetworkErrorCode(call, hr, hr);
            XAsyncComplete(asyncBlock, S_OK, 0);
            return;
        }

        if (requestBodySize > 0 && requestBodyReadFunction != nullptr)
        {
            auto requestStream = Microsoft::WRL::Make<http_request_stream>();
            if (requestStream != nullptr)
            {
                hr = requestStream->init(call);
            }
            else
            {
                hr = E_OUTOFMEMORY;
            }

            if (FAILED(hr))
            {
                HC_TRACE_ERROR(HTTPCLIENT, "[%d] http_request_stream failed in xmlhttp_http_task.", hr);
                HCHttpCallResponseSetNetworkErrorCode(call, E_FAIL, static_cast<uint32_t>(hr));
                XAsyncComplete(asyncBlock, S_OK, 0);
                return;
            }

            hr = m_hRequest->Send(requestStream.Get(), requestBodySize);
        }
        else
        {
            hr = m_hRequest->Send(nullptr, 0);
        }

        if (FAILED(hr))
        {
            HC_TRACE_ERROR(HTTPCLIENT, "Failure to send HTTP request 0x%0.8x", hr);
            HCHttpCallResponseSetNetworkErrorCode(call, hr, hr);
            XAsyncComplete(asyncBlock, S_OK, 0);
            return;
        }
        // If there were no errors so far, HCTaskSetCompleted is called later 
        // http_request_callback::OnResponseReceived 
        // or 
        // http_request_callback::OnError
    }
    catch (std::bad_alloc const& e)
    {
        HC_TRACE_ERROR(HTTPCLIENT, "[%d] std::bad_alloc in xmlhttp_http_task: %s", E_OUTOFMEMORY, e.what());
        HCHttpCallResponseSetNetworkErrorCode(call, E_OUTOFMEMORY, static_cast<uint32_t>(E_OUTOFMEMORY));
        XAsyncComplete(asyncBlock, E_OUTOFMEMORY, 0);
    }
    catch (std::exception const& e)
    {
        HC_TRACE_ERROR(HTTPCLIENT, "[%d] std::exception in xmlhttp_http_task: %s", E_FAIL, e.what());
        HCHttpCallResponseSetNetworkErrorCode(call, E_FAIL, static_cast<uint32_t>(E_FAIL));
        XAsyncComplete(asyncBlock, E_FAIL, 0);
    }
    catch (...)
    {
        HC_TRACE_ERROR(HTTPCLIENT, "[%d] unknown exception in xmlhttp_http_task", E_FAIL);
        HCHttpCallResponseSetNetworkErrorCode(call, E_FAIL, static_cast<uint32_t>(E_FAIL));
        XAsyncComplete(asyncBlock, E_FAIL, 0);
    }
}

void xmlhttp_http_task::set_status_code(_In_ uint32_t statusCode)
{
    m_statusCode = statusCode;
}

uint32_t xmlhttp_http_task::get_status_code()
{
    return m_statusCode;
}

http_internal_vector<http_internal_string> xmlhttp_http_task::split(
    _In_ const http_internal_string& s, 
    _In_z_ const char* delim)
{
    http_internal_vector<http_internal_string> v;
    size_t i = 0;
    size_t delimLen = strlen(delim);
    auto pos = s.find(delim);
    while (pos != std::string::npos) 
    {
        v.push_back(s.substr(i, pos - i));
        i = pos + delimLen;
        pos = s.find(delim, i);
    }
    v.push_back(s.substr(i, s.length()));

    return v;
}

void xmlhttp_http_task::set_headers(_In_ WCHAR* allResponseHeaders)
{
    auto allHeaders = utf8_from_utf16(allResponseHeaders);
    auto splitHeaders = split(allHeaders, "\r\n");
    for (auto& header : splitHeaders)
    {
        auto colonPos = header.find(':');
        if (colonPos == std::string::npos || colonPos == 0)
        {
            // Invalid header found
            continue;
        }

        m_headerNames.push_back(header.substr(0, colonPos));
        size_t valueStartPos = colonPos + 1; // skip the colon
        valueStartPos = header.find_first_not_of(" \t", valueStartPos); // skip all leading optional whitespace
        if (valueStartPos != std::string::npos)
        {
            size_t valueEndPos = header.find_last_not_of(" \t");
            m_headerValues.push_back(header.substr(valueStartPos, valueEndPos - valueStartPos + 1));
        }
        else
        {
            m_headerValues.push_back("");
        }
    }

    ASSERT(m_headerNames.size() == m_headerValues.size());
}

const http_internal_vector<http_internal_string>& xmlhttp_http_task::get_headers_names()
{
    return m_headerNames;
}

const http_internal_vector<http_internal_string>& xmlhttp_http_task::get_headers_values()
{
    return m_headerValues;
}

bool xmlhttp_http_task::has_error()
{
    return m_exceptionPtr != nullptr;
}

void xmlhttp_http_task::set_exception(const std::exception_ptr& exceptionPtr)
{
    m_exceptionPtr = exceptionPtr;
}

HCCallHandle xmlhttp_http_task::call()
{
    return m_call;
}

XAsyncBlock* xmlhttp_http_task::async_block()
{
    return m_asyncBlock;
}

#include "urls.h"

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

static std::atomic<int> queuedRequests;
static win32_handle g_stopRequestedHandle;
static win32_handle g_workReadyHandle;
static win32_handle g_completionReadyHandle;

static XTaskQueueHandle m_queue;
static XTaskQueueRegistrationToken m_callbackToken;


static std::atomic<int> processedRequests, processedSize;

int firstCallTime;

void MakeHttpCallSingleX(const char* url)
{
	static bool first = true;
	if (first)
	{
		first = false;
		firstCallTime = GetTickCount();
	}
	queuedRequests++;

	std::string requestBody = "";
	std::string requestMethod = "GET";
	std::string requestUrl = url;
	bool retryAllowed = true;

	HCCallHandle call = nullptr;
	HCHttpCallCreate(&call);
	HCHttpCallRequestSetUrl(call, requestMethod.c_str(), requestUrl.c_str());

	HCHttpCallRequestSetRetryAllowed(call, retryAllowed);
	HCHttpCallRequestSetHeader(call, "User-Agent", "XboxServicesAPI", true);
	HCHttpCallRequestSetHeader(call, "x-xbl-contract-version", "1", true);

	XAsyncBlock* asyncBlock = new XAsyncBlock;
	ZeroMemory(asyncBlock, sizeof(XAsyncBlock));
	asyncBlock->context = call;
	asyncBlock->queue = m_queue;
	asyncBlock->callback = [](XAsyncBlock* asyncBlock)
	{
		HCCallHandle call = static_cast<HCCallHandle>(asyncBlock->context);

		size_t bsize = 0;
		HCHttpCallResponseGetResponseBodyBytesSize(call, &bsize);
		HCHttpCallCloseHandle(call);

		std::wstringstream ss;

		size_t newSize = (processedSize += (int)bsize);
		int reqId = processedRequests++;

		int curT = GetTickCount();
		ss << L"Response " << reqId << " size: " << bsize << " total size: " << newSize << " qsize: " << queuedRequests << "t: " << (curT - firstCallTime) << "\n";
		OutputDebugString(ss.str().c_str());

		delete asyncBlock;

		queuedRequests--;

		if (sizeof(urls) / sizeof(urls[0]) == reqId + 1)
		{
            ss.clear();
			ss << L"Total time " << (curT - firstCallTime) << " count: " << (reqId + 1) << " total size: " << newSize << "\n";
			OutputDebugString(ss.str().c_str());
		}
	};

	HCHttpCallPerformAsync(call, asyncBlock);
}


static std::deque<const char*> uqueue(urls, urls + sizeof(urls) / sizeof(urls[0]));

void MakeHttpCallTick()
{
	while (queuedRequests < 16 && !uqueue.empty())
	{
		auto url = uqueue.front();
		uqueue.pop_front();
		MakeHttpCallSingleX(url);
	}
}



static void CALLBACK HandleAsyncQueueCallback(
	_In_ void* context,
	_In_ XTaskQueueHandle queue,
	_In_ XTaskQueuePort type
)
{
	UNREFERENCED_PARAMETER(context);
	UNREFERENCED_PARAMETER(queue);

	switch (type)
	{
	case XTaskQueuePort::Work:
		SetEvent(g_workReadyHandle.get());
		break;

	case XTaskQueuePort::Completion:
		SetEvent(g_completionReadyHandle.get());
		break;
	}
}

static DWORD WINAPI background_thread_proc(LPVOID lpParam)
{
	HANDLE hEvents[3] =
	{
		g_workReadyHandle.get(),
		g_completionReadyHandle.get(),
		g_stopRequestedHandle.get()
	};

	XTaskQueueHandle queue = static_cast<XTaskQueueHandle>(lpParam);

	UNREFERENCED_PARAMETER(lpParam);
	bool stop = false;
	while (!stop)
	{
		DWORD dwResult = WaitForMultipleObjectsEx(3, hEvents, false, INFINITE, false);

		switch (dwResult)
		{
		case WAIT_OBJECT_0: // work ready
			if (XTaskQueueDispatch(queue, XTaskQueuePort::Work, 0))
			{
				// If there's more pending work, then set the event to process them
				SetEvent(g_workReadyHandle.get());
			}
			break;

		case WAIT_OBJECT_0 + 1: // completed 
			// Typically completions should be dispatched on the game thread, but
			// for this simple XAML app we're doing it here
			if (XTaskQueueDispatch(queue, XTaskQueuePort::Completion, 0))
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

HANDLE m_hBackgroundThread;

void StartBackgroundThread()
{
	if (m_hBackgroundThread == nullptr)
	{
		m_hBackgroundThread = CreateThread(nullptr, 0, background_thread_proc, m_queue, 0, nullptr);
	}
}

void HttpPerfTest()
{
	g_stopRequestedHandle.set(CreateEvent(nullptr, true, false, nullptr));
	g_workReadyHandle.set(CreateEvent(nullptr, false, false, nullptr));
	g_completionReadyHandle.set(CreateEvent(nullptr, false, false, nullptr));

	HCInitialize(nullptr);
	HCSettingsSetTraceLevel(HCTraceLevel::Verbose);

	XTaskQueueCreate(
		XTaskQueueDispatchMode::Manual,
		XTaskQueueDispatchMode::Manual,
		&m_queue);
	XTaskQueueRegisterMonitor(m_queue, nullptr, HandleAsyncQueueCallback, &m_callbackToken);

	StartBackgroundThread();

	Sleep(16);

	std::thread([] {
		while (!uqueue.empty())
		{
			MakeHttpCallTick();
			Sleep(1);
		}
	}).detach();

	Sleep(1000000);
}
