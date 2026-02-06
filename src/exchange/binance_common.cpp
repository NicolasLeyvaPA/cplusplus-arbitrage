#include "exchange/binance_common.hpp"
#include "utils/crypto.hpp"
#include <spdlog/spdlog.h>
#include <openssl/hmac.h>
#include <openssl/evp.h>
#include <curl/curl.h>
#include <sstream>
#include <iomanip>
#include <stdexcept>

namespace arb {

BinanceHttpClient::BinanceHttpClient(const std::string& base_url,
                                     const std::string& api_key,
                                     const std::string& api_secret)
    : base_url_(base_url)
    , api_key_(api_key)
    , api_secret_(api_secret)
{
    spdlog::info("BinanceHttpClient initialized: {}", base_url);
}

BinanceHttpClient::~BinanceHttpClient() = default;

size_t BinanceHttpClient::write_callback(char* ptr, size_t size, size_t nmemb, std::string* data) {
    data->append(ptr, size * nmemb);
    return size * nmemb;
}

std::string BinanceHttpClient::build_query_string(const std::map<std::string, std::string>& params) {
    std::string qs;
    for (const auto& [key, value] : params) {
        if (!qs.empty()) qs += "&";
        qs += key + "=" + value;
    }
    return qs;
}

std::string BinanceHttpClient::sign_query(const std::string& query_string) {
    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int hash_len;

    HMAC(EVP_sha256(),
         api_secret_.data(), static_cast<int>(api_secret_.size()),
         reinterpret_cast<const unsigned char*>(query_string.data()),
         query_string.size(),
         hash, &hash_len);

    std::stringstream ss;
    for (unsigned int i = 0; i < hash_len; i++) {
        ss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(hash[i]);
    }
    return ss.str();
}

nlohmann::json BinanceHttpClient::perform_request(const std::string& method,
                                                    const std::string& url,
                                                    bool add_auth_header) {
    std::lock_guard<std::mutex> lock(request_mutex_);
    rate_limiter_.acquire();

    CURL* curl = curl_easy_init();
    if (!curl) {
        throw std::runtime_error("Failed to initialize CURL");
    }

    std::string response_body;
    struct curl_slist* headers = nullptr;

    if (add_auth_header && !api_key_.empty()) {
        std::string auth_header = "X-MBX-APIKEY: " + api_key_;
        headers = curl_slist_append(headers, auth_header.c_str());
    }
    headers = curl_slist_append(headers, "Content-Type: application/x-www-form-urlencoded");

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);

    if (method == "POST") {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, "");
    } else if (method == "DELETE") {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
    }

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        throw std::runtime_error("CURL error: " + std::string(curl_easy_strerror(res)));
    }

    nlohmann::json j;
    try {
        j = nlohmann::json::parse(response_body);
    } catch (...) {
        throw std::runtime_error("Failed to parse JSON: " + response_body.substr(0, 200));
    }

    if (http_code >= 400) {
        std::string msg = j.contains("msg") ? j["msg"].get<std::string>() : response_body;
        int code = j.contains("code") ? j["code"].get<int>() : static_cast<int>(http_code);
        spdlog::error("Binance API error {}: {}", code, msg);
        throw std::runtime_error("Binance API error " + std::to_string(code) + ": " + msg);
    }

    return j;
}

nlohmann::json BinanceHttpClient::public_get(const std::string& endpoint,
                                              const std::map<std::string, std::string>& params) {
    std::string url = base_url_ + endpoint;
    std::string qs = build_query_string(params);
    if (!qs.empty()) url += "?" + qs;
    return perform_request("GET", url, false);
}

nlohmann::json BinanceHttpClient::signed_get(const std::string& endpoint,
                                              const std::map<std::string, std::string>& params) {
    auto signed_params = params;
    signed_params["timestamp"] = std::to_string(now_ms());
    std::string qs = build_query_string(signed_params);
    std::string signature = sign_query(qs);
    qs += "&signature=" + signature;
    std::string url = base_url_ + endpoint + "?" + qs;
    return perform_request("GET", url, true);
}

nlohmann::json BinanceHttpClient::signed_post(const std::string& endpoint,
                                               const std::map<std::string, std::string>& params) {
    auto signed_params = params;
    signed_params["timestamp"] = std::to_string(now_ms());
    std::string qs = build_query_string(signed_params);
    std::string signature = sign_query(qs);
    qs += "&signature=" + signature;
    std::string url = base_url_ + endpoint + "?" + qs;
    return perform_request("POST", url, true);
}

nlohmann::json BinanceHttpClient::signed_delete(const std::string& endpoint,
                                                 const std::map<std::string, std::string>& params) {
    auto signed_params = params;
    signed_params["timestamp"] = std::to_string(now_ms());
    std::string qs = build_query_string(signed_params);
    std::string signature = sign_query(qs);
    qs += "&signature=" + signature;
    std::string url = base_url_ + endpoint + "?" + qs;
    return perform_request("DELETE", url, true);
}

} // namespace arb
