#include "kubelet/kube_api_client.hpp"
#include <iostream>
#include <cassert>
#include <vector>
#include <string>
#include <random>
#include <chrono>

using kube32::kubelet::HttpKubeClient;

void test_truncated_chunks() {
    std::cout << "[TEST] Truncated chunk streams..." << std::endl;

    // 1. Incomplete hex size
    assert(HttpKubeClient::decode_chunked_body("A") == "");
    assert(HttpKubeClient::decode_chunked_body("12") == "");
    assert(HttpKubeClient::decode_chunked_body("DEAD") == "");
    assert(HttpKubeClient::decode_chunked_body("") == "");

    // 2. Incomplete data body (chunk_size > available)
    // When size is 16 (0x10) but only 5 bytes provided: partial copy up to available
    std::string partial1 = HttpKubeClient::decode_chunked_body("10\r\n12345");
    assert(partial1 == "12345");

    std::string partial2 = HttpKubeClient::decode_chunked_body("10\n12345");
    assert(partial2 == "12345");

    // 3. Size provided, zero body bytes
    assert(HttpKubeClient::decode_chunked_body("10\r\n") == "");
    assert(HttpKubeClient::decode_chunked_body("10\n") == "");

    // 4. Incomplete terminal chunk
    assert(HttpKubeClient::decode_chunked_body("0") == "");
    assert(HttpKubeClient::decode_chunked_body("0\r\n") == "");
    assert(HttpKubeClient::decode_chunked_body("0\r\n\r\n") == "");

    // 5. Multi-chunk truncated at 2nd chunk
    std::string multi_trunc = "5\r\nhello\r\n10\r\nworld";
    std::string res = HttpKubeClient::decode_chunked_body(multi_trunc);
    assert(res == "helloworld");

    std::cout << "  [PASS] Truncated chunk streams handled cleanly without crash." << std::endl;
}

void test_malformed_chunk_sizes() {
    std::cout << "[TEST] Malformed chunk sizes..." << std::endl;

    // Non-hex characters
    assert(HttpKubeClient::decode_chunked_body("0x10\r\nhello\r\n0\r\n\r\n") == "");
    assert(HttpKubeClient::decode_chunked_body("ZZ\r\nhello\r\n0\r\n\r\n") == "");
    assert(HttpKubeClient::decode_chunked_body("-5\r\nhello\r\n0\r\n\r\n") == "");
    assert(HttpKubeClient::decode_chunked_body("+5\r\nhello\r\n0\r\n\r\n") == "");
    assert(HttpKubeClient::decode_chunked_body("hello\r\nhello\r\n0\r\n\r\n") == "");
    assert(HttpKubeClient::decode_chunked_body(" 5\r\nhello\r\n0\r\n\r\n") == "hello"); // Whitespace before size stripped by RFC-tolerant parser

    // Extreme/overflow hex numbers: bounded by available buffer, no crash/hang
    assert(HttpKubeClient::decode_chunked_body("FFFFFFFFFFFFFFFF\r\nhello\r\n0\r\n\r\n") == "hello\r\n0\r\n\r\n");
    assert(HttpKubeClient::decode_chunked_body("FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF\r\nhello\r\n0\r\n\r\n") == "hello\r\n0\r\n\r\n");

    // Leading newlines
    std::string leading_nl = "\r\n\r\n5\r\nhello\r\n0\r\n\r\n";
    assert(HttpKubeClient::decode_chunked_body(leading_nl) == "hello");

    // Extensions with weird characters
    std::string weird_ext = "5;foo=bar;baz=\"quoted;semi\"\r\nhello\r\n0;term-ext\r\n\r\n";
    assert(HttpKubeClient::decode_chunked_body(weird_ext) == "hello");

    // Huge extension line (8KB of extensions)
    std::string big_ext = "5;" + std::string(8192, 'x') + "\r\nhello\r\n0\r\n\r\n";
    assert(HttpKubeClient::decode_chunked_body(big_ext) == "hello");

    std::cout << "  [PASS] Malformed chunk sizes and extensions handled cleanly." << std::endl;
}

void test_embedded_null_bytes() {
    std::cout << "[TEST] Embedded null bytes..." << std::endl;

    // Null byte in chunk data
    std::string with_null = "5\r\nhe\0lo\r\n0\r\n\r\n";
    std::string s_with_null(with_null.data(), 14);
    std::string dec = HttpKubeClient::decode_chunked_body(s_with_null);
    assert(dec.size() == 5);
    assert(dec[2] == '\0');

    // Null byte in hex size (should be rejected as invalid hex)
    std::string null_hex = "5\0\r\nhello\r\n0\r\n\r\n";
    std::string s_null_hex(null_hex.data(), 14);
    assert(HttpKubeClient::decode_chunked_body(s_null_hex) == "");

    std::cout << "  [PASS] Embedded null bytes handled safely." << std::endl;
}

void test_fuzzing_stress(size_t iterations = 50000) {
    std::cout << "[TEST] Fuzzing stress (" << iterations << " iterations)..." << std::endl;

    std::mt19937 rng(42);
    std::uniform_int_distribution<size_t> len_dist(0, 1024);
    std::uniform_int_distribution<int> byte_dist(0, 255);

    auto start_time = std::chrono::steady_clock::now();

    for (size_t i = 0; i < iterations; ++i) {
        size_t len = len_dist(rng);
        std::string fuzz_data;
        fuzz_data.resize(len);
        for (size_t j = 0; j < len; ++j) {
            fuzz_data[j] = static_cast<char>(byte_dist(rng));
        }

        // Must never throw, crash, segfault, or hang
        std::string out = HttpKubeClient::decode_chunked_body(fuzz_data);
        assert(out.size() <= len);

        // Also test header detection fuzz
        bool is_chunked = HttpKubeClient::is_chunked_transfer_encoding(fuzz_data);
        (void)is_chunked;
    }

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_time).count();
    std::cout << "  [PASS] " << iterations << " fuzzed inputs completed in " << elapsed << "ms without crash or hang." << std::endl;
}

void test_header_detection_edge_cases() {
    std::cout << "[TEST] Header detection edge cases..." << std::endl;

    assert(HttpKubeClient::is_chunked_transfer_encoding("Transfer-Encoding: chunked\r\n"));
    assert(HttpKubeClient::is_chunked_transfer_encoding("Transfer-Encoding: chunked\n"));
    assert(HttpKubeClient::is_chunked_transfer_encoding("TRANSFER-ENCODING: CHUNKED\r\n"));
    assert(HttpKubeClient::is_chunked_transfer_encoding("tRaNsFeR-eNcOdInG: cHuNkEd\r\n"));
    assert(HttpKubeClient::is_chunked_transfer_encoding("Transfer-Encoding: gzip, chunked\r\n"));
    assert(HttpKubeClient::is_chunked_transfer_encoding("Transfer-Encoding: chunked, gzip\r\n"));
    assert(HttpKubeClient::is_chunked_transfer_encoding("Transfer-Encoding:   chunked  \r\n"));
    assert(HttpKubeClient::is_chunked_transfer_encoding("Transfer-Encoding: chunked; ext=val\r\n"));
    assert(!HttpKubeClient::is_chunked_transfer_encoding("Transfer-Encoding: gzip\r\n"));
    assert(!HttpKubeClient::is_chunked_transfer_encoding("Transfer-Encoding: identity\r\n"));
    assert(!HttpKubeClient::is_chunked_transfer_encoding("Content-Length: 1234\r\n"));
    assert(!HttpKubeClient::is_chunked_transfer_encoding(""));
    assert(!HttpKubeClient::is_chunked_transfer_encoding("\r\n\r\n"));
    assert(!HttpKubeClient::is_chunked_transfer_encoding("X-Transfer-Encoding: chunked\r\n"));

    std::cout << "  [PASS] Header detection edge cases passed." << std::endl;
}

int main() {
    std::cout << "==========================================================" << std::endl;
    std::cout << " Kube32 HTTP Chunked Decoding Adversarial Stress Test     " << std::endl;
    std::cout << "==========================================================" << std::endl;

    test_truncated_chunks();
    test_malformed_chunk_sizes();
    test_embedded_null_bytes();
    test_header_detection_edge_cases();
    test_fuzzing_stress(50000);

    std::cout << "==========================================================" << std::endl;
    std::cout << " ALL CHUNKED DECODING STRESS TESTS: PASSED!              " << std::endl;
    std::cout << "==========================================================" << std::endl;
    return 0;
}
