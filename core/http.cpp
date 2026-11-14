#include "core/http.h"

#include <iterator>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>
#endif

namespace hx {

#if defined(_WIN32)

namespace {

std::wstring wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(std::size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

struct Handle {
    HINTERNET h = nullptr;
    ~Handle() {
        if (h) WinHttpCloseHandle(h);
    }
};

bool fail(std::string* error, const std::string& what) {
    if (error) *error = what + " (WinHTTP error " + std::to_string(GetLastError()) + ")";
    return false;
}

} // namespace

bool httpGet(const std::string& url, std::vector<std::uint8_t>& body, std::string* error, std::size_t maxBytes) {
    body.clear();
    const std::wstring wurl = wide(url);
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof parts;
    wchar_t host[256] = {}, path[2048] = {};
    parts.lpszHostName = host;
    parts.dwHostNameLength = DWORD(std::size(host));
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = DWORD(std::size(path));
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &parts)) return fail(error, "bad URL " + url);
    // Plain identification: no user data in the request.
    Handle session{WinHttpOpen(L"oshexed", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                               WINHTTP_NO_PROXY_BYPASS, 0)};
    if (!session.h) return fail(error, "no HTTP session");
    WinHttpSetTimeouts(session.h, 10000, 10000, 15000, 30000);
    Handle connect{WinHttpConnect(session.h, host, parts.nPort, 0)};
    if (!connect.h) return fail(error, "can't reach " + url);
    const DWORD flags = parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
    Handle request{WinHttpOpenRequest(connect.h, L"GET", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags)};
    if (!request.h) return fail(error, "can't request " + url);
    if (!WinHttpSendRequest(request.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.h, nullptr))
        return fail(error, "no answer from " + url);
    DWORD status = 0, len = sizeof status;
    WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &status, &len, WINHTTP_NO_HEADER_INDEX);
    if (status != 200) {
        if (error) *error = url + ": HTTP " + std::to_string(status);
        return false;
    }
    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request.h, &available)) return fail(error, "download of " + url + " broke off");
        if (available == 0) break;
        if (body.size() + available > maxBytes) {
            if (error) *error = url + ": larger than expected";
            body.clear();
            return false;
        }
        const std::size_t at = body.size();
        body.resize(at + available);
        DWORD read = 0;
        if (!WinHttpReadData(request.h, body.data() + at, available, &read)) return fail(error, "download of " + url + " broke off");
        body.resize(at + read);
    }
    return true;
}

#else

bool httpGet(const std::string& url, std::vector<std::uint8_t>& body, std::string* error, std::size_t) {
    body.clear();
    if (error) *error = "downloads aren't available on this platform yet (" + url + ")";
    return false;
}

#endif

} // namespace hx
