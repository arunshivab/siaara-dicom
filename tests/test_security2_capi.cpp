// Regression tests for the 2026-11-10 02:15 security review, C API logging (R-075, R-076).
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "capi/capi_internal.hpp"
#include "siaara/siaara.h"
#include "test.hpp"

namespace {
struct Seen {
    std::mutex mu;
    std::vector<std::string> text;
    std::vector<int> phi;
};
void SIAARA_CALL record(void* user, int32_t, int32_t phi, const char* m) {
    auto* s = static_cast<Seen*>(user);
    std::lock_guard<std::mutex> l(s->mu);
    s->text.emplace_back(m);
    s->phi.push_back(phi);
}
struct Slow {
    std::atomic<bool> entered{false}, finished{false};
};
void SIAARA_CALL slow(void* user, int32_t, int32_t, const char*) {
    auto* s = static_cast<Slow*>(user);
    s->entered = true;
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    s->finished = true;
}
}  // namespace

TEST_CASE("R-075: PHI-flagged log messages are withheld unless include_phi is set") {
    Seen seen;
    REQUIRE(siaara_set_log_callback(record, &seen, SIAARA_LOG_DEBUG) == SIAARA_OK);
    siaara::capi::log(SIAARA_LOG_WARNING, true, "instance 2.25.123 of DOE^JANE");
    siaara::capi::log(SIAARA_LOG_WARNING, false, "association from 127.0.0.1 released");
    REQUIRE(siaara_set_log_include_phi(1) == SIAARA_OK);
    siaara::capi::log(SIAARA_LOG_WARNING, true, "instance 2.25.124 of DOE^JOHN");
    REQUIRE(siaara_set_log_include_phi(0) == SIAARA_OK);
    REQUIRE(siaara_set_log_callback(nullptr, nullptr, SIAARA_LOG_DEBUG) == SIAARA_OK);
    CHECK(siaara_set_log_include_phi(2) == SIAARA_ERR_INVALID_ARGUMENT);
    std::lock_guard<std::mutex> l(seen.mu);
    REQUIRE(seen.text.size() == 3u);
    CHECK(seen.text[0].find("DOE") == std::string::npos);
    CHECK(seen.text[0].find("2.25.123") == std::string::npos);
    CHECK_EQ(seen.phi[0], 0);
    CHECK(seen.text[1].find("127.0.0.1") != std::string::npos);
    CHECK(seen.text[2].find("DOE^JOHN") != std::string::npos);
    CHECK_EQ(seen.phi[2], 1);
}

TEST_CASE("R-076: replacing the log callback waits for calls of the old one") {
    Slow st;
    REQUIRE(siaara_set_log_callback(slow, &st, SIAARA_LOG_DEBUG) == SIAARA_OK);
    std::thread t([] { siaara::capi::log(SIAARA_LOG_INFO, false, "x"); });
    while (!st.entered.load()) std::this_thread::yield();
    REQUIRE(siaara_set_log_callback(nullptr, nullptr, SIAARA_LOG_DEBUG) == SIAARA_OK);
    CHECK(st.finished.load());  // `user` may be freed now
    t.join();
}
