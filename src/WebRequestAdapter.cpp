#include "WebRequestAdapter.h"
#include <algorithm>
#include <cstring>
#include <deque>
#include <vector>

#include <ESPAsyncWebServer.h>
#if defined(ARDUINO_ARCH_ESP32)
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>
#endif
#include "PsramAlloc.h"
#include "PsramLimits.h"
#include "UnifiedLogger.h"

namespace webserver {

namespace {
WebRequestMethodComposite toAsyncHttpMethod(RequestMethod method) {
    switch (method) {
        case RequestMethod::Any: return HTTP_ANY;
        case RequestMethod::Get: return HTTP_GET;
        case RequestMethod::Post: return HTTP_POST;
        case RequestMethod::Put: return HTTP_PUT;
        case RequestMethod::Patch: return HTTP_PATCH;
        case RequestMethod::Delete: return HTTP_DELETE;
        case RequestMethod::Options: return HTTP_OPTIONS;
        case RequestMethod::Head: return HTTP_HEAD;
        case RequestMethod::Unknown: return HTTP_ANY;
        default: return HTTP_ANY;
    }
}

RequestMethod toRequestMethodAsync(WebRequestMethod method) {
    switch (method) {
        case HTTP_GET: return RequestMethod::Get;
        case HTTP_POST: return RequestMethod::Post;
        case HTTP_PUT: return RequestMethod::Put;
        case HTTP_PATCH: return RequestMethod::Patch;
        case HTTP_DELETE: return RequestMethod::Delete;
        case HTTP_OPTIONS: return RequestMethod::Options;
        case HTTP_HEAD: return RequestMethod::Head;
        default: return RequestMethod::Unknown;
    }
}

size_t gResponseBufferPoolBytesInUse = 0;

#if defined(ARDUINO_ARCH_ESP32)
portMUX_TYPE gResponseBufferPoolMux = portMUX_INITIALIZER_UNLOCKED;
#define RESPONSE_POOL_LOCK() portENTER_CRITICAL(&gResponseBufferPoolMux)
#define RESPONSE_POOL_UNLOCK() portEXIT_CRITICAL(&gResponseBufferPoolMux)
#else
#define RESPONSE_POOL_LOCK()
#define RESPONSE_POOL_UNLOCK()
#endif

bool reserveResponseBufferPool(size_t bytes) {
    if (bytes == 0) {
        return true;
    }

    RESPONSE_POOL_LOCK();
    const bool canReserve = gResponseBufferPoolBytesInUse <= psram_limits::kResponseChunkPoolBytes &&
                            bytes <= (psram_limits::kResponseChunkPoolBytes - gResponseBufferPoolBytesInUse);
    if (canReserve) {
        gResponseBufferPoolBytesInUse += bytes;
    }
    RESPONSE_POOL_UNLOCK();
    return canReserve;
}

void releaseResponseBufferPool(size_t bytes) {
    if (bytes == 0) {
        return;
    }

    RESPONSE_POOL_LOCK();
    if (bytes <= gResponseBufferPoolBytesInUse) {
        gResponseBufferPoolBytesInUse -= bytes;
    } else {
        gResponseBufferPoolBytesInUse = 0;
    }
    RESPONSE_POOL_UNLOCK();
}

class AsyncWebRequestAdapter final : public IRequest {
public:
    AsyncWebRequestAdapter(AsyncWebServerRequest& requestRef, String requestBody)
        : request(requestRef),
          bodyData(std::move(requestBody)) {
    }

    RequestMethod method() override {
        return toRequestMethodAsync(request.method());
    }

    String uri() override {
        return request.url();
    }

    bool hasArg(const String& name) override {
        if (name == "plain") {
            return bodyData.length() > 0 || request.contentLength() > 0;
        }
        return request.hasArg(name);
    }

    String arg(const String& name) override {
        if (name == "plain") {
            return bodyData;
        }
        return request.arg(name);
    }

    String body() override {
        return bodyData;
    }

    IPAddress remoteIP() override {
        if (request.client()) {
            return request.client()->remoteIP();
        }
        return IPAddress();
    }

private:
    AsyncWebServerRequest& request;
    String bodyData;
};

class AsyncWebResponseAdapter final : public IResponse {
public:
    explicit AsyncWebResponseAdapter(AsyncWebServerRequest& requestRef)
        : request(requestRef) {
    }

    ~AsyncWebResponseAdapter() override {
        flushDeferredResponseIfNeeded();
    }

    void send(int statusCode, const String& contentType, const String& body) override {
        if (responseSent) {
            return;
        }

        if (body.isEmpty() && contentLengthHintSet) {
            deferredResponsePending = true;
            deferredStatusCode = statusCode;
            deferredContentType = contentType;
            deferredMode = (contentLengthHint == kContentLengthUnknown) ? DeferredMode::Chunked : DeferredMode::FixedBody;
            if (!chunkedState) {
                chunkedState = std::shared_ptr<ChunkedState>(new ChunkedState());
            }
            return;
        }

        AsyncWebServerResponse* response = request.beginResponse(statusCode, contentType, body);
        if (!response) {
            return;
        }
        applyPendingHeaders(response);
        request.send(response);
        responseSent = true;
    }

    void send(int statusCode) override {
        send(statusCode, "text/plain; charset=UTF-8", "");
    }

    void sendHeader(const String& name, const String& value) override {
        if (responseSent || name.isEmpty()) {
            return;
        }
        pendingHeaders.push_back({name, value});
    }

    size_t streamFile(File& file, const String& contentType) override {
        if (responseSent || !file) {
            return 0;
        }

        String path = file.name();
        if (path.isEmpty()) {
            path = "/file.bin";
        }

        const size_t fileSize = file.size();
        AsyncWebServerResponse* response = request.beginResponse(file, path, contentType);
        if (!response) {
            return 0;
        }
        applyPendingHeaders(response);
        request.send(response);
        responseSent = true;
        return fileSize;
    }

    size_t sendFile(FS& fs, const char* path, const String& contentType) override {
        if (responseSent || !path) {
            return 0;
        }

        size_t fileSize = 0;
        {
            File probe = fs.open(path, "r");
            if (!probe) {
                return 0;
            }
            fileSize = probe.size();
            probe.close();
        }

        AsyncWebServerResponse* response = request.beginResponse(fs, path, contentType, false);
        if (!response) {
            return 0;
        }
        applyPendingHeaders(response);
        request.send(response);
        responseSent = true;
        return fileSize;
    }

    void setContentLength(size_t contentLength) override {
        contentLengthHintSet = true;
        contentLengthHint = contentLength;
    }

    void sendContent(const String& content) override {
        if (responseSent) {
            return;
        }
        if (deferredStorageFailed) {
            return;
        }

        if (!deferredResponsePending) {
            deferredResponsePending = true;
            deferredStatusCode = 200;
            deferredContentType = "text/plain; charset=UTF-8";
            deferredMode = DeferredMode::FixedBody;
        }

        if (!chunkedState) {
            chunkedState = std::shared_ptr<ChunkedState>(new ChunkedState());
        }

        if (content.length() == 0) {
            return;
        }

        if (!chunkedState->append(content.c_str(), static_cast<size_t>(content.length()))) {
            deferredStorageFailed = true;
            UnifiedLogger::warning("[WEBADAPTER] response staging overflow for %s (response bytes=%u, pool bytes=%u)\n",
                                   request.url().c_str(),
                                   static_cast<unsigned int>(chunkedState->bufferedBytes),
                                   static_cast<unsigned int>(gResponseBufferPoolBytesInUse));
        }
    }

    bool authenticate(const char* username, const char* password) override {
        return request.authenticate(username, password, nullptr, false);
    }

    void requestAuthentication() override {
        request.requestAuthentication(AsyncAuthType::AUTH_BASIC, nullptr);
    }

private:
    enum class DeferredMode : uint8_t {
        None = 0,
        FixedBody,
        Chunked
    };

    struct ChunkedState {
        struct Chunk {
            char* data = nullptr;
            size_t length = 0;
            size_t offset = 0;
        };

        ~ChunkedState() {
            clear();
        }

        bool append(const char* src, size_t len) {
            if (!src || len == 0) {
                return true;
            }

            if ((bufferedBytes + len) > psram_limits::kResponseChunkMaxPerResponseBytes) {
                return false;
            }
            if (!reserveResponseBufferPool(len)) {
                return false;
            }

            psram::AllocationResult allocResult = psram::allocate(len);
            if (!allocResult.ok()) {
                releaseResponseBufferPool(len);
                return false;
            }

            Chunk chunk;
            chunk.data = static_cast<char*>(allocResult.ptr);
            chunk.length = len;
            chunk.offset = 0;
            memcpy(chunk.data, src, len);

            chunks.push_back(chunk);
            bufferedBytes += len;
            return true;
        }

        bool copyToString(String& out) {
            if (bufferedBytes > 0) {
                out.reserve(bufferedBytes);
            }
            for (const Chunk& chunk : chunks) {
                if (chunk.data && chunk.length > 0) {
                    out.concat(chunk.data + chunk.offset,
                               static_cast<unsigned int>(chunk.length - chunk.offset));
                }
            }
            return true;
        }

        size_t fill(uint8_t* buffer, size_t maxLen) {
            if (!buffer || maxLen == 0) {
                return 0;
            }

            size_t written = 0;
            while (written < maxLen) {
                if (chunks.empty()) {
                    if (finalized) {
                        return written;
                    }
                    return (written > 0) ? written : RESPONSE_TRY_AGAIN;
                }

                Chunk& chunk = chunks.front();
                if (!chunk.data || chunk.offset >= chunk.length) {
                    popFront();
                    continue;
                }

                const size_t remaining = chunk.length - chunk.offset;
                const size_t toCopy = std::min(maxLen - written, remaining);
                memcpy(buffer + written, chunk.data + chunk.offset, toCopy);
                written += toCopy;
                chunk.offset += toCopy;

                if (chunk.offset >= chunk.length) {
                    popFront();
                }
            }

            return written;
        }

        void clear() {
            while (!chunks.empty()) {
                popFront();
            }
            bufferedBytes = 0;
            finalized = false;
        }

        void popFront() {
            if (chunks.empty()) {
                return;
            }

            Chunk chunk = chunks.front();
            chunks.pop_front();

            if (chunk.data) {
                psram::deallocate(chunk.data);
                releaseResponseBufferPool(chunk.length);
            }
            if (chunk.length <= bufferedBytes) {
                bufferedBytes -= chunk.length;
            } else {
                bufferedBytes = 0;
            }
        }

        std::deque<Chunk> chunks;
        size_t bufferedBytes = 0;
        bool finalized = false;
    };

    struct PendingHeader {
        String name;
        String value;
    };

    static size_t fillChunkedBuffer(const std::shared_ptr<ChunkedState>& state, uint8_t* buffer, size_t maxLen) {
        if (!state || !buffer || maxLen == 0) {
            return 0;
        }
        return state->fill(buffer, maxLen);
    }

    void applyPendingHeaders(AsyncWebServerResponse* response) {
        if (!response) {
            return;
        }

        for (const PendingHeader& header : pendingHeaders) {
            response->addHeader(header.name, header.value);
        }
    }

    void flushDeferredResponseIfNeeded() {
        if (responseSent || !deferredResponsePending) {
            return;
        }

        if (deferredStorageFailed) {
            chunkedState.reset();
            AsyncWebServerResponse* response = request.beginResponse(
                500,
                "application/json; charset=UTF-8",
                "{\"error\":\"response_buffer_overflow\"}");
            if (!response) {
                return;
            }
            applyPendingHeaders(response);
            request.send(response);
            responseSent = true;
            return;
        }

        if (deferredMode == DeferredMode::Chunked) {
            if (!chunkedState) {
                chunkedState = std::shared_ptr<ChunkedState>(new ChunkedState());
            }
            chunkedState->finalized = true;
            const std::shared_ptr<ChunkedState> state = chunkedState;

            AsyncWebServerResponse* response = request.beginChunkedResponse(
                deferredContentType.isEmpty() ? "text/plain; charset=UTF-8" : deferredContentType,
                [state](uint8_t* buffer, size_t maxLen, size_t index) -> size_t {
                    (void)index;
                    return fillChunkedBuffer(state, buffer, maxLen);
                });
            if (!response) {
                return;
            }

            response->setCode(deferredStatusCode);
            applyPendingHeaders(response);
            request.send(response);
            responseSent = true;
            return;
        }

        String body;
        if (chunkedState) {
            chunkedState->copyToString(body);
        }
        AsyncWebServerResponse* response = request.beginResponse(
            deferredStatusCode,
            deferredContentType.isEmpty() ? "text/plain; charset=UTF-8" : deferredContentType,
            body);
        if (!response) {
            return;
        }

        applyPendingHeaders(response);
        request.send(response);
        responseSent = true;
        chunkedState.reset();
    }

    AsyncWebServerRequest& request;
    std::vector<PendingHeader> pendingHeaders;
    bool responseSent = false;
    bool contentLengthHintSet = false;
    size_t contentLengthHint = 0;
    bool deferredResponsePending = false;
    DeferredMode deferredMode = DeferredMode::None;
    int deferredStatusCode = 200;
    String deferredContentType;
    bool deferredStorageFailed = false;
    std::shared_ptr<ChunkedState> chunkedState;
};

class AsyncWebServerBackend final : public IWebServerBackend {
public:
    explicit AsyncWebServerBackend(uint16_t port)
        : server(port) {
    }

    ~AsyncWebServerBackend() override {
        clearBodyBuffers();
    }

    WebServerBackendKind kind() const override {
        return WebServerBackendKind::Async;
    }

    void begin() override {
        server.begin();
    }

    void poll() override {
        // Async backend has no per-loop polling requirement.
    }

    void registerRoute(const char* uri, RequestMethod method, const ExchangeHandler& handler) override {
        AsyncURIMatcher routeMatcher = AsyncURIMatcher::exact(uri ? uri : "");
        server.on(
            std::move(routeMatcher),
            toAsyncHttpMethod(method),
            [this, handler](AsyncWebServerRequest* request) {
                const RequestBodyReadResult requestBody = takeRequestBody(request);
                if (requestBody.truncated) {
                    AsyncWebResponseAdapter responseAdapter(*request);
                    responseAdapter.send(413, "application/json; charset=UTF-8", "{\"error\":\"Request body too large\"}");
                    return;
                }

                AsyncWebRequestAdapter requestAdapter(*request, requestBody.body);
                AsyncWebResponseAdapter responseAdapter(*request);
                handler(requestAdapter, responseAdapter);
            },
            nullptr,
            [this](AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
                appendRequestBody(request, data, len, index, total);
            });
    }

private:
    struct BodyBufferEntry {
        AsyncWebServerRequest* request = nullptr;
        char* data = nullptr;
        size_t length = 0;
        size_t capacity = 0;
        unsigned long updatedAtMs = 0;
        size_t totalExpected = 0;
        bool truncated = false;
        psram::AllocationTier tier = psram::AllocationTier::None;
    };

    struct RequestBodyReadResult {
        String body;
        bool truncated = false;
    };

    static constexpr unsigned long BODY_BUFFER_STALE_MS = 30000UL;
    static constexpr size_t MAX_BODY_BUFFER_ENTRIES = 24;
    static constexpr size_t BODY_POOL_MAX_BYTES = psram_limits::kRequestBodyPoolBytes;
    static constexpr size_t BODY_MAX_PER_REQUEST_BYTES = psram_limits::kRequestBodyMaxPerRequestBytes;

    void clearBodyBuffers() {
        for (BodyBufferEntry& entry : bodyBuffers) {
            releaseBodyEntryStorage(entry);
        }
        bodyBuffers.clear();
        bodyBufferBytesInUse = 0;
    }

    void releaseBodyEntryStorage(BodyBufferEntry& entry) {
        if (entry.data) {
            psram::deallocate(entry.data);
            if (entry.capacity <= bodyBufferBytesInUse) {
                bodyBufferBytesInUse -= entry.capacity;
            } else {
                bodyBufferBytesInUse = 0;
            }
        }

        entry.data = nullptr;
        entry.length = 0;
        entry.capacity = 0;
        entry.totalExpected = 0;
        entry.truncated = false;
        entry.tier = psram::AllocationTier::None;
    }

    void removeBodyBufferAt(size_t index) {
        if (index >= bodyBuffers.size()) {
            return;
        }
        releaseBodyEntryStorage(bodyBuffers[index]);
        bodyBuffers.erase(bodyBuffers.begin() + static_cast<long>(index));
    }

    void removeBodyBuffer(AsyncWebServerRequest* request) {
        if (!request) {
            return;
        }
        for (size_t i = 0; i < bodyBuffers.size(); ++i) {
            if (bodyBuffers[i].request == request) {
                removeBodyBufferAt(i);
                return;
            }
        }
    }

    void pruneBodyBuffers(unsigned long nowMs) {
        for (size_t i = 0; i < bodyBuffers.size();) {
            const BodyBufferEntry& entry = bodyBuffers[i];
            const bool stale = !entry.request ||
                               static_cast<unsigned long>(nowMs - entry.updatedAtMs) > BODY_BUFFER_STALE_MS;
            if (stale) {
                removeBodyBufferAt(i);
                continue;
            }
            ++i;
        }

        while (bodyBuffers.size() > MAX_BODY_BUFFER_ENTRIES) {
            removeBodyBufferAt(0);
        }
    }

    BodyBufferEntry* findBodyBuffer(AsyncWebServerRequest* request) {
        for (BodyBufferEntry& entry : bodyBuffers) {
            if (entry.request == request) {
                return &entry;
            }
        }
        return nullptr;
    }

    bool reserveBodyBuffer(BodyBufferEntry& entry, size_t requiredBytes) {
        if (requiredBytes <= entry.capacity) {
            return true;
        }

        if (requiredBytes > BODY_MAX_PER_REQUEST_BYTES) {
            return false;
        }

        size_t newCapacity = (entry.capacity > 0) ? entry.capacity : 256U;
        while (newCapacity < requiredBytes && newCapacity < BODY_MAX_PER_REQUEST_BYTES) {
            newCapacity *= 2U;
        }
        if (newCapacity < requiredBytes) {
            newCapacity = requiredBytes;
        }
        if (newCapacity > BODY_MAX_PER_REQUEST_BYTES) {
            newCapacity = BODY_MAX_PER_REQUEST_BYTES;
        }
        if (newCapacity < requiredBytes) {
            return false;
        }

        const size_t reclaimableCurrent = entry.capacity;
        const size_t freePoolBytes = (bodyBufferBytesInUse < BODY_POOL_MAX_BYTES)
                                         ? (BODY_POOL_MAX_BYTES - bodyBufferBytesInUse)
                                         : 0;
        if (newCapacity > (reclaimableCurrent + freePoolBytes)) {
            return false;
        }

        psram::AllocationResult allocResult;
        if (entry.data) {
            allocResult = psram::reallocate(entry.data, newCapacity);
        } else {
            allocResult = psram::allocate(newCapacity);
        }
        if (!allocResult.ok()) {
            return false;
        }

        if (newCapacity >= entry.capacity) {
            bodyBufferBytesInUse += (newCapacity - entry.capacity);
        } else {
            const size_t delta = entry.capacity - newCapacity;
            bodyBufferBytesInUse = (delta <= bodyBufferBytesInUse) ? (bodyBufferBytesInUse - delta) : 0;
        }

        entry.data = static_cast<char*>(allocResult.ptr);
        entry.capacity = newCapacity;
        entry.tier = allocResult.tier;
        return true;
    }

    void appendRequestBody(AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
        (void)total;
        if (!request || !data || len == 0) {
            return;
        }

        const unsigned long nowMs = millis();
        pruneBodyBuffers(nowMs);

        BodyBufferEntry* entry = findBodyBuffer(request);
        if (index == 0) {
            if (!entry) {
                if (bodyBuffers.size() >= MAX_BODY_BUFFER_ENTRIES) {
                    pruneBodyBuffers(nowMs);
                }
                if (bodyBuffers.size() >= MAX_BODY_BUFFER_ENTRIES) {
                    removeBodyBufferAt(0);
                }
                bodyBuffers.push_back({});
                entry = &bodyBuffers.back();
                entry->request = request;
                request->onDisconnect([this, request]() {
                    removeBodyBuffer(request);
                });
            }
            entry->request = request;
            entry->length = 0;
            entry->totalExpected = total;
            entry->truncated = false;
            if (total > BODY_MAX_PER_REQUEST_BYTES) {
                entry->truncated = true;
                UnifiedLogger::warning("[WEBADAPTER] request body exceeds limit (%u > %u) for %s\n",
                                       static_cast<unsigned int>(total),
                                       static_cast<unsigned int>(BODY_MAX_PER_REQUEST_BYTES),
                                       request->url().c_str());
            }
        } else if (!entry) {
            if (bodyBuffers.size() >= MAX_BODY_BUFFER_ENTRIES) {
                pruneBodyBuffers(nowMs);
            }
            if (bodyBuffers.size() >= MAX_BODY_BUFFER_ENTRIES) {
                removeBodyBufferAt(0);
            }
            bodyBuffers.push_back({});
            entry = &bodyBuffers.back();
            entry->request = request;
            request->onDisconnect([this, request]() {
                removeBodyBuffer(request);
            });
            entry->totalExpected = total;
        }

        entry->updatedAtMs = nowMs;
        if (entry->truncated) {
            return;
        }

        if ((entry->length + len) > BODY_MAX_PER_REQUEST_BYTES) {
            entry->truncated = true;
            UnifiedLogger::warning("[WEBADAPTER] request body truncated at %u bytes for %s\n",
                                   static_cast<unsigned int>(BODY_MAX_PER_REQUEST_BYTES),
                                   request->url().c_str());
            return;
        }

        const size_t targetLength = entry->length + len;
        if (!reserveBodyBuffer(*entry, targetLength)) {
            entry->truncated = true;
            UnifiedLogger::warning("[WEBADAPTER] request body allocation failed (pool=%u/%u bytes) for %s\n",
                                   static_cast<unsigned int>(bodyBufferBytesInUse),
                                   static_cast<unsigned int>(BODY_POOL_MAX_BYTES),
                                   request->url().c_str());
            return;
        }

        memcpy(entry->data + entry->length, data, len);
        entry->length = targetLength;
    }

    RequestBodyReadResult takeRequestBody(AsyncWebServerRequest* request) {
        RequestBodyReadResult result;
        pruneBodyBuffers(millis());
        for (size_t i = 0; i < bodyBuffers.size(); ++i) {
            if (bodyBuffers[i].request == request) {
                BodyBufferEntry& entry = bodyBuffers[i];
                result.truncated = entry.truncated;
                if (!entry.truncated && entry.data && entry.length > 0) {
                    result.body.reserve(entry.length);
                    result.body.concat(entry.data, static_cast<unsigned int>(entry.length));
                }
                removeBodyBufferAt(i);
                return result;
            }
        }
        return result;
    }

    AsyncWebServer server;
    std::vector<BodyBufferEntry> bodyBuffers;
    size_t bodyBufferBytesInUse = 0;
};
} // namespace

std::unique_ptr<IWebServerBackend> createWebServerBackend(WebServerBackendKind kind, uint16_t port) {
    switch (kind) {
        case WebServerBackendKind::Async:
            return std::unique_ptr<IWebServerBackend>(new AsyncWebServerBackend(port));
        default:
            return nullptr;
    }
}

const char* webServerBackendKindName(WebServerBackendKind kind) {
    switch (kind) {
        case WebServerBackendKind::Async: return "async";
        default: return "unknown";
    }
}

namespace {
bool isTerminalControlCommandStatus(MainController::ControlCommandStatus status) {
    return status == MainController::ControlCommandStatus::SUCCEEDED ||
           status == MainController::ControlCommandStatus::FAILED ||
           status == MainController::ControlCommandStatus::TIMED_OUT;
}

ControlDispatchWaitResult waitForQueuedControlCommand(
    MainController* mainController,
    uint32_t commandId,
    unsigned long timeoutMs,
    unsigned long pollIntervalMs) {
    ControlDispatchWaitResult result;
    result.commandId = commandId;

    const unsigned long startedAtMs = millis();
    while ((millis() - startedAtMs) <= timeoutMs) {
        MainController::ControlCommandResult commandResult;
        if (mainController->getControlCommandResult(commandId, commandResult)) {
            result.commandResult = commandResult;
            if (isTerminalControlCommandStatus(commandResult.status)) {
                result.status = ControlDispatchWaitStatus::Completed;
                (void)mainController->clearControlCommandResult(commandId);
                return result;
            }
        }

        if (pollIntervalMs > 0) {
            delay(pollIntervalMs);
        } else {
            yield();
        }
    }

    MainController::ControlCommandResult commandResult;
    if (mainController->getControlCommandResult(commandId, commandResult)) {
        result.commandResult = commandResult;
    }
    result.status = ControlDispatchWaitStatus::TimedOut;
    return result;
}
} // namespace

ControlDispatchWaitResult dispatchControlCommandAndWait(
    MainController* mainController,
    const MainController::ControlCommand& command,
    unsigned long timeoutMs,
    unsigned long pollIntervalMs) {
    ControlDispatchWaitResult result;

    if (!mainController) {
        result.status = ControlDispatchWaitStatus::ControllerUnavailable;
        UnifiedLogger::error("[WEBADAPTER] Control dispatch failed: MainController unavailable\n");
        return result;
    }

    uint32_t commandId = 0;
    if (!mainController->enqueueControlCommand(command, commandId)) {
        result.status = ControlDispatchWaitStatus::EnqueueRejected;
        UnifiedLogger::warning("[WEBADAPTER] Control dispatch rejected by queue\n");
        return result;
    }
    result = waitForQueuedControlCommand(mainController, commandId, timeoutMs, pollIntervalMs);
    if (result.status == ControlDispatchWaitStatus::TimedOut) {
        UnifiedLogger::warning("[WEBADAPTER] Control dispatch timed out (commandId=%lu)\n",
                               static_cast<unsigned long>(commandId));
    }
    return result;
}

ControlDispatchWaitResult dispatchSettingsUpdateAndWait(
    MainController* mainController,
    const String& jsonBody,
    unsigned long timeoutMs,
    unsigned long pollIntervalMs) {
    ControlDispatchWaitResult result;

    if (!mainController) {
        result.status = ControlDispatchWaitStatus::ControllerUnavailable;
        UnifiedLogger::error("[WEBADAPTER] Settings dispatch failed: MainController unavailable\n");
        return result;
    }

    uint32_t commandId = 0;
    String enqueueError;
    if (!mainController->enqueueSettingsUpdateCommand(jsonBody, commandId, enqueueError)) {
        result.status = ControlDispatchWaitStatus::EnqueueRejected;
        result.commandId = commandId;
        result.commandResult.commandId = commandId;
        result.commandResult.status = MainController::ControlCommandStatus::FAILED;
        result.commandResult.errorCode = -7;
        result.commandResult.updatedAtMs = millis();
        strncpy(result.commandResult.message, enqueueError.c_str(), sizeof(result.commandResult.message) - 1);
        result.commandResult.message[sizeof(result.commandResult.message) - 1] = '\0';
        UnifiedLogger::warning("[WEBADAPTER] Settings dispatch rejected: %s\n",
                               enqueueError.length() ? enqueueError.c_str() : "queue rejected");
        return result;
    }
    result = waitForQueuedControlCommand(mainController, commandId, timeoutMs, pollIntervalMs);
    if (result.status == ControlDispatchWaitStatus::TimedOut) {
        UnifiedLogger::warning("[WEBADAPTER] Settings dispatch timed out (commandId=%lu)\n",
                               static_cast<unsigned long>(commandId));
    }
    return result;
}

} // namespace webserver
