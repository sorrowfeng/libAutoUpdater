#pragma once

#include "libAutoUpdater/interfaces/INetworkClient.h"

#include <deque>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace autoupdater::test {

struct ScriptedResponse {
    int statusCode = 200;
    std::vector<NetworkHeader> headers;
    std::string body;
    std::optional<std::string> effectiveUrl;
    std::string downloadedBytes;
};

class ScriptedNetworkClient final : public INetworkClient {
  public:
    void queueText(std::string url, ScriptedResponse response) {
        textResponses_[std::move(url)].push_back(std::move(response));
    }

    void queueDownload(std::string url, ScriptedResponse response) {
        downloadResponses_[std::move(url)].push_back(std::move(response));
    }

    Result<TextResponse> getText(const std::string& url, const NetworkOptions&, std::uint64_t maxResponseBytes,
                                 CancellationToken&) noexcept override {
        textRequests.push_back(url);
        textLimits.push_back(maxResponseBytes);
        auto scripted = take(textResponses_, url);
        if (!scripted) {
            return Result<TextResponse>::fail(scripted.error());
        }

        TextResponse response;
        response.response.statusCode = scripted.value().statusCode;
        response.response.headers = std::move(scripted.value().headers);
        response.response.effectiveUrl = scripted.value().effectiveUrl ? *scripted.value().effectiveUrl : url;
        response.body = std::move(scripted.value().body);
        return Result<TextResponse>::ok(std::move(response));
    }

    Result<DownloadResult> downloadToFile(const std::string& url, IRootedFile& target, const NetworkOptions&,
                                          std::uint64_t maxTotalBytes, const std::optional<DownloadResumeInfo>& resume,
                                          ProgressCallback, CancellationToken&) noexcept override {
        downloadRequests.push_back(url);
        downloadLimits.push_back(maxTotalBytes);
        downloadResumes.push_back(resume);
        auto scripted = take(downloadResponses_, url);
        if (!scripted) {
            return Result<DownloadResult>::fail(scripted.error());
        }
        if (!scripted.value().downloadedBytes.empty()) {
            auto written =
                target.write(scripted.value().downloadedBytes.data(), scripted.value().downloadedBytes.size());
            if (!written) {
                return Result<DownloadResult>::fail(written.error());
            }
        }
        auto metadata = target.metadata();
        if (!metadata) {
            return Result<DownloadResult>::fail(metadata.error());
        }
        downloadTargetSizesAfterResponse.push_back(metadata.value().size);

        DownloadResult result;
        result.response.statusCode = scripted.value().statusCode;
        result.response.headers = std::move(scripted.value().headers);
        result.response.effectiveUrl = scripted.value().effectiveUrl ? *scripted.value().effectiveUrl : url;
        result.bytesWritten = scripted.value().downloadedBytes.size();
        return Result<DownloadResult>::ok(std::move(result));
    }

    std::vector<std::string> textRequests;
    std::vector<std::uint64_t> textLimits;
    std::vector<std::string> downloadRequests;
    std::vector<std::uint64_t> downloadLimits;
    std::vector<std::optional<DownloadResumeInfo>> downloadResumes;
    std::vector<std::uint64_t> downloadTargetSizesAfterResponse;

  private:
    using Responses = std::map<std::string, std::deque<ScriptedResponse>>;

    static Result<ScriptedResponse> take(Responses& responses, const std::string& url) {
        auto found = responses.find(url);
        if (found == responses.end() || found->second.empty()) {
            return Result<ScriptedResponse>::fail({ErrorCode::NetworkError, "Unexpected request: " + url});
        }
        auto response = std::move(found->second.front());
        found->second.pop_front();
        return Result<ScriptedResponse>::ok(std::move(response));
    }

    Responses textResponses_;
    Responses downloadResponses_;
};

inline ScriptedResponse textResponse(std::string body, int statusCode = 200) {
    ScriptedResponse response;
    response.statusCode = statusCode;
    response.body = std::move(body);
    return response;
}

inline ScriptedResponse redirectResponse(int statusCode, std::string location) {
    ScriptedResponse response;
    response.statusCode = statusCode;
    response.headers.push_back({"Location", std::move(location)});
    return response;
}

} // namespace autoupdater::test
