#include <gtest/gtest.h>

#include <string>

#include "emberdb/http.hpp"

using emberdb::HttpParse;
using emberdb::http_response;
using emberdb::parse_http_request;

TEST(HttpRequestParsing, ParsesASimpleGet) {
    const auto r =
        parse_http_request("GET /metrics HTTP/1.1\r\nHost: x\r\nUser-Agent: curl\r\n\r\n");
    ASSERT_EQ(r.status, HttpParse::kComplete);
    EXPECT_EQ(r.request.method, "GET");
    EXPECT_EQ(r.request.path, "/metrics");
}

TEST(HttpRequestParsing, NeedsTheBlankLine) {
    EXPECT_EQ(parse_http_request("").status, HttpParse::kIncomplete);
    EXPECT_EQ(parse_http_request("GET /x HTTP/1.1\r\n").status, HttpParse::kIncomplete);
    EXPECT_EQ(parse_http_request("GET /x HTTP/1.1\r\nHost: x\r\n").status, HttpParse::kIncomplete);
    EXPECT_EQ(parse_http_request("GET /x HTTP/1.1\r\n\r\n").status, HttpParse::kComplete);
}

TEST(HttpRequestParsing, StripsQueryAndFragment) {
    EXPECT_EQ(parse_http_request("GET /metrics?x=1&y=2 HTTP/1.1\r\n\r\n").request.path, "/metrics");
    EXPECT_EQ(parse_http_request("GET /a#frag HTTP/1.0\r\n\r\n").request.path, "/a");
    EXPECT_EQ(parse_http_request("GET /?q HTTP/1.1\r\n\r\n").request.path, "/");
}

TEST(HttpRequestParsing, DoesNotDecodePercentEscapes) {
    EXPECT_EQ(parse_http_request("GET /%6Detrics HTTP/1.1\r\n\r\n").request.path, "/%6Detrics");
}

TEST(HttpRequestParsing, RejectsMalformedRequestLines) {
    const char* bad_lines[] = {
        "GET /",                 // no version
        "GET / HTTP/2",          // unsupported version
        "get / HTTP/1.1",        // the method must be uppercase
        "GET metrics HTTP/1.1",  // the target must start with a slash
        "GET  / HTTP/1.1",       // two spaces
        " / HTTP/1.1",           // no method
        "GET / HTTP/1.1 extra",  // junk after the version
        "",                      // empty
    };
    for (const char* line : bad_lines) {
        const std::string request = std::string(line) + "\r\n\r\n";
        EXPECT_EQ(parse_http_request(request).status, HttpParse::kBad) << line;
    }
}

TEST(HttpRequestParsing, RejectsControlAndNonAsciiBytesInTheTarget) {
    EXPECT_EQ(parse_http_request(std::string("GET /\x01") + "x HTTP/1.1\r\n\r\n").status,
              HttpParse::kBad);
    EXPECT_EQ(parse_http_request("GET /\x7f HTTP/1.1\r\n\r\n").status, HttpParse::kBad);
    EXPECT_EQ(parse_http_request("GET /\xff HTTP/1.1\r\n\r\n").status, HttpParse::kBad);
}

TEST(HttpResponses, HasStatusLineHeadersAndBody) {
    const std::string r = http_response(200, "text/plain", "hello\n");
    EXPECT_TRUE(r.starts_with("HTTP/1.1 200 OK\r\n"));
    EXPECT_NE(r.find("Content-Type: text/plain\r\n"), std::string::npos);
    EXPECT_NE(r.find("Content-Length: 6\r\n"), std::string::npos);
    EXPECT_NE(r.find("Connection: close\r\n"), std::string::npos);
    EXPECT_NE(r.find("Cache-Control: no-store\r\n"), std::string::npos);
    EXPECT_NE(r.find("X-Content-Type-Options: nosniff\r\n"), std::string::npos);
    EXPECT_TRUE(r.ends_with("\r\n\r\nhello\n"));
}

TEST(HttpResponses, KnowsItsStatusCodes) {
    EXPECT_TRUE(http_response(400, "text/plain", "").starts_with("HTTP/1.1 400 Bad Request\r\n"));
    EXPECT_TRUE(http_response(404, "text/plain", "").starts_with("HTTP/1.1 404 Not Found\r\n"));
    EXPECT_TRUE(http_response(405, "text/plain", "")
                    .starts_with("HTTP/1.1 405 Method Not Allowed\r\n"));
    EXPECT_TRUE(http_response(431, "text/plain", "")
                    .starts_with("HTTP/1.1 431 Request Header Fields Too Large\r\n"));
    EXPECT_TRUE(http_response(503, "text/plain", "")
                    .starts_with("HTTP/1.1 503 Service Unavailable\r\n"));
}

TEST(HttpResponses, ExtraHeadersComeBeforeTheBlankLine) {
    const std::string r = http_response(405, "text/plain", "no\n", "Allow: GET\r\n");
    const std::size_t allow = r.find("Allow: GET\r\n");
    const std::size_t blank = r.find("\r\n\r\n");
    ASSERT_NE(allow, std::string::npos);
    EXPECT_LT(allow, blank);
    EXPECT_EQ(blank, r.rfind("\r\n\r\n"));  // exactly one blank line: headers end, then the body
}
