#include "http_client.h"
#include "log_entry.h"
#include <lib/logger/src/logger.h>
#include <lib/tagging/src/tagging_registry.h>

#include <algorithm>
#include <utility>
#include <thread>

#include <curl/curl.h>

namespace atlasagent {

  template class HttpClient<atlasagent::TaggingRegistry>;
  template class HttpClient<spectator::SpectatordRegistry>;

class CurlHeaders {
 public:
  CurlHeaders() = default;
  ~CurlHeaders() { curl_slist_free_all(list_); }
  CurlHeaders(const CurlHeaders&) = delete;
  CurlHeaders(CurlHeaders&&) = delete;
  CurlHeaders& operator=(const CurlHeaders&) = delete;
  CurlHeaders& operator=(CurlHeaders&&) = delete;
  void append(const std::string& string) { list_ = curl_slist_append(list_, string.c_str()); }
  curl_slist* headers() { return list_; }

 private:
  curl_slist* list_{nullptr};
};

namespace detail {

constexpr const char* const kUserAgent = "atlas-system-agent/1.0";
inline size_t curl_ignore_output_fun(char* /*unused*/, size_t size, size_t nmemb,
                                     void* /*unused*/) {
  return size * nmemb;
}

inline size_t curl_capture_output_fun(char* contents, size_t size, size_t nmemb, void* userp) {
  auto real_size = size * nmemb;
  auto* resp = static_cast<std::string*>(userp);
  resp->append(contents, real_size);
  return real_size;
}

inline size_t curl_capture_headers_fun(char* contents, size_t size, size_t nmemb, void* userp) {
  auto real_size = size * nmemb;
  auto end = contents + real_size;
  auto* headers = static_cast<HttpHeaders*>(userp);
  // see if it's a proper header and not HTTP/xx or the final \n
  auto p = static_cast<char*>(memchr(contents, ':', real_size));
  if (p != nullptr && p + 2 < end) {
    std::string key{contents, p};
    std::string value{p + 2, end - 1};  // drop last lf
    headers->emplace(std::make_pair(std::move(key), std::move(value)));
  }
  return real_size;
}

class CurlHandle {
 public:
  CurlHandle() noexcept : handle_{curl_easy_init()} {
    curl_easy_setopt(handle_, CURLOPT_USERAGENT, kUserAgent);
  }

  CurlHandle(const CurlHandle&) = delete;

  CurlHandle& operator=(const CurlHandle&) = delete;

  CurlHandle(CurlHandle&& other) = delete;

  CurlHandle& operator=(CurlHandle&& other) = delete;

  ~CurlHandle() {
    // nullptr is handled by curl
    curl_easy_cleanup(handle_);
  }

  CURL* handle() const noexcept { return handle_; }

  CURLcode perform() { return curl_easy_perform(handle()); }

  CURLcode set_opt(CURLoption option, const void* param) {
    return curl_easy_setopt(handle(), option, param);
  }

  int status_code() const {
    // curl requires this to be a long
    long http_code = 400;
    curl_easy_getinfo(handle(), CURLINFO_RESPONSE_CODE, &http_code);
    return static_cast<int>(http_code);
  }

  std::string response() const { return response_; }

  void move_response(std::string* out) { *out = std::move(response_); }

  HttpHeaders headers() const { return resp_headers_; }

  void move_headers(HttpHeaders* out) { *out = std::move(resp_headers_); }

  void set_url(const std::string& url) { set_opt(CURLOPT_URL, url.c_str()); }

  void set_headers(std::shared_ptr<CurlHeaders> headers) {
    headers_ = std::move(headers);
    set_opt(CURLOPT_HTTPHEADER, headers_->headers());
  }

  void set_connect_timeout(absl::Duration connect_timeout) {
    auto millis = static_cast<long>(absl::ToInt64Milliseconds(connect_timeout));
    curl_easy_setopt(handle_, CURLOPT_CONNECTTIMEOUT_MS, millis);
  }

  void set_timeout(absl::Duration total_timeout) {
    auto millis = static_cast<long>(absl::ToInt64Milliseconds(total_timeout));
    curl_easy_setopt(handle_, CURLOPT_TIMEOUT_MS, millis);
  }

  void post_payload(const void* payload, size_t size) {
    payload_ = payload;
    curl_easy_setopt(handle_, CURLOPT_POST, 1L);
    curl_easy_setopt(handle_, CURLOPT_POSTFIELDS, payload_);
    curl_easy_setopt(handle_, CURLOPT_POSTFIELDSIZE, size);
  }

  void custom_request(const char* method) {
    curl_easy_setopt(handle_, CURLOPT_CUSTOMREQUEST, method);
  }

  void ignore_output() { curl_easy_setopt(handle_, CURLOPT_WRITEFUNCTION, curl_ignore_output_fun); }

  void capture_output() {
    curl_easy_setopt(handle_, CURLOPT_WRITEFUNCTION, curl_capture_output_fun);
    curl_easy_setopt(handle_, CURLOPT_WRITEDATA, static_cast<void*>(&response_));
  }

  void capture_headers() {
    curl_easy_setopt(handle_, CURLOPT_HEADERDATA, static_cast<void*>(&resp_headers_));
    curl_easy_setopt(handle_, CURLOPT_HEADERFUNCTION, curl_capture_headers_fun);
  }

 private:
  CURL* handle_;
  std::shared_ptr<CurlHeaders> headers_;
  const void* payload_ = nullptr;
  std::string response_;
  HttpHeaders resp_headers_;
};

}  // namespace detail

template <typename Reg>
HttpResponse HttpClient<Reg>::method_header(const char* method, const std::string& url,
                                            const std::vector<std::string>& headers) const {
  auto curl_headers = std::make_shared<CurlHeaders>();
  for (const auto& h : headers) {
    curl_headers->append(h);
  }
  return perform(method, url, std::move(curl_headers), nullptr, 0u, 0);
}

inline bool is_retryable_error(int http_code) { return http_code == 429 || (http_code / 100) == 5; }

template <typename Reg>
HttpResponse HttpClient<Reg>::perform(const char* method, const std::string& url,
                                      std::shared_ptr<CurlHeaders> headers, const char* payload,
                                      size_t size, uint32_t attempt_number) const {
  LogEntry entry{registry_, method, url};

  detail::CurlHandle curl;
  auto total_timeout = config_.connect_timeout + config_.read_timeout;
  curl.set_timeout(total_timeout);
  curl.set_connect_timeout(config_.connect_timeout);

  auto logger = Logger();
  curl.set_url(url);
  curl.set_headers(headers);
  if (strcmp("POST", method) == 0) {
    curl.post_payload(payload, size);
  } else if (strcmp("GET", method) != 0) {
    curl.custom_request(method);
  }
  curl.capture_output();
  curl.capture_headers();

  auto curl_res = curl.perform();
  auto http_code = 400;

  if (curl_res != CURLE_OK) {
    logger->error("Failed to {} {}: {}", method, url, curl_easy_strerror(curl_res));
    switch (curl_res) {
      case CURLE_COULDNT_CONNECT:
        entry.set_error("connection_error");
        break;
      case CURLE_OPERATION_TIMEDOUT:
        entry.set_error("timeout");
        break;
      default:
        entry.set_error("unknown");
    }
    auto elapsed = absl::Now() - entry.start();
    // retry connect timeouts if possible, not read timeouts
    auto connect_to = absl::ToInt64Milliseconds(config_.connect_timeout);
    auto read_to = absl::ToInt64Milliseconds(config_.read_timeout);
    logger->info("HTTP timeout to {}: {}ms elapsed - connect_to={} read_to={}", url,
                 absl::ToInt64Milliseconds(elapsed), connect_to, read_to);
    if (elapsed < total_timeout && attempt_number < 2) {
      entry.set_attempt(attempt_number, false);
      entry.log();
      return perform(method, url, std::move(headers), payload, size, attempt_number + 1);
    }

    entry.set_status_code(-1);
  } else {
    http_code = curl.status_code();
    entry.set_status_code(http_code);
    if (http_code / 100 == 2) {
      entry.set_success();
    } else {
      entry.set_error("http_error");
    }
    if (is_retryable_error(http_code) && attempt_number < 2) {
      logger->info("Got a retryable http code from {}: {} (attempt {})", url, http_code,
                   attempt_number);
      entry.set_attempt(attempt_number, false);
      entry.log();
      auto sleep_ms = uint32_t(200) << attempt_number;  // 200, 400ms
      std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
      return perform(method, url, std::move(headers), payload, size, attempt_number + 1);
    }
    logger->debug("{} {} - status code: {}", method, url, http_code);
  }
  entry.set_attempt(attempt_number, true);
  entry.log();

  std::string resp;
  curl.move_response(&resp);

  HttpHeaders resp_headers;
  curl.move_headers(&resp_headers);
  return HttpResponse{http_code, std::move(resp), std::move(resp_headers)};
}

template <typename Reg>
void HttpClient<Reg>::GlobalInit() noexcept {
  static bool init = false;
  if (init) {
    return;
  }

  init = true;
  curl_global_init(CURL_GLOBAL_ALL);
}

template <typename Reg>
void HttpClient<Reg>::GlobalShutdown() noexcept {
  static bool shutdown = false;
  if (shutdown) {
    return;
  }
  shutdown = true;
  curl_global_cleanup();
}

}  // namespace atlasagent
