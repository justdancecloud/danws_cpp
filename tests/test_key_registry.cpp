#include "danws/state/key_registry.h"
#include "danws/protocol/error.h"

#include <cassert>
#include <iostream>
#include <string>

using namespace danws;

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) \
    static void test_##name(); \
    static struct TestReg_##name { \
        TestReg_##name() { \
            std::cout << "  " #name "... "; \
            try { test_##name(); tests_passed++; std::cout << "OK\n"; } \
            catch (const std::exception& e) { tests_failed++; std::cout << "FAIL: " << e.what() << "\n"; } \
        } \
    } test_reg_##name; \
    static void test_##name()

TEST(register_and_lookup) {
    KeyRegistry reg;
    reg.registerOne(1, "user.name", DataType::String);
    reg.registerOne(2, "user.age", DataType::VarInteger);

    assert(reg.size() == 2);
    assert(reg.hasKeyId(1));
    assert(reg.hasKeyId(2));
    assert(!reg.hasKeyId(3));

    auto* entry = reg.getByKeyId(1);
    assert(entry != nullptr);
    assert(entry->path == "user.name");
    assert(entry->type == DataType::String);
    assert(entry->keyId == 1);

    auto* entry2 = reg.getByPath("user.age");
    assert(entry2 != nullptr);
    assert(entry2->keyId == 2);
}

TEST(lookup_by_path) {
    KeyRegistry reg;
    reg.registerOne(5, "sensor.temp", DataType::VarDouble);

    assert(reg.hasPath("sensor.temp"));
    assert(!reg.hasPath("sensor.humidity"));

    auto* entry = reg.getByPath("sensor.temp");
    assert(entry != nullptr);
    assert(entry->keyId == 5);
}

TEST(remove_by_keyid) {
    KeyRegistry reg;
    reg.registerOne(1, "key1", DataType::String);
    reg.registerOne(2, "key2", DataType::Bool);

    assert(reg.size() == 2);
    assert(reg.removeByKeyId(1));
    assert(reg.size() == 1);
    assert(!reg.hasKeyId(1));
    assert(!reg.hasPath("key1"));
    assert(reg.hasKeyId(2));
}

TEST(remove_nonexistent) {
    KeyRegistry reg;
    assert(!reg.removeByKeyId(999));
}

TEST(clear) {
    KeyRegistry reg;
    reg.registerOne(1, "a", DataType::Null);
    reg.registerOne(2, "b", DataType::Null);
    assert(reg.size() == 2);

    reg.clear();
    assert(reg.size() == 0);
    assert(!reg.hasKeyId(1));
    assert(!reg.hasPath("a"));
}

TEST(paths_list) {
    KeyRegistry reg;
    reg.registerOne(1, "alpha", DataType::String);
    reg.registerOne(2, "beta", DataType::Int32);
    reg.registerOne(3, "gamma", DataType::Bool);

    auto paths = reg.paths();
    assert(paths.size() == 3);
    // Check that all paths are present (order not guaranteed)
    bool hasAlpha = false, hasBeta = false, hasGamma = false;
    for (const auto& p : paths) {
        if (p == "alpha") hasAlpha = true;
        if (p == "beta") hasBeta = true;
        if (p == "gamma") hasGamma = true;
    }
    assert(hasAlpha && hasBeta && hasGamma);
}

TEST(validate_empty_path) {
    bool threw = false;
    try {
        validateKeyPath("");
    } catch (const DanWSError& e) {
        threw = true;
        assert(std::string(e.code()) == "INVALID_KEY_PATH");
    }
    assert(threw);
}

TEST(validate_invalid_chars) {
    bool threw = false;
    try {
        validateKeyPath("key with spaces");
    } catch (const DanWSError&) {
        threw = true;
    }
    assert(threw);
}

TEST(validate_dot_path) {
    // Valid dot-separated path
    validateKeyPath("user.name");
    validateKeyPath("a.b.c.d");
    validateKeyPath("root_key");
    validateKeyPath("a123");
}

TEST(validate_leading_dot) {
    bool threw = false;
    try {
        validateKeyPath(".leading");
    } catch (const DanWSError&) {
        threw = true;
    }
    assert(threw);
}

TEST(key_limit) {
    KeyRegistry reg(3);
    reg.registerOne(1, "a", DataType::Null);
    reg.registerOne(2, "b", DataType::Null);
    reg.registerOne(3, "c", DataType::Null);

    bool threw = false;
    try {
        reg.registerOne(4, "d", DataType::Null);
    } catch (const DanWSError& e) {
        threw = true;
        assert(std::string(e.code()) == "KEY_LIMIT_EXCEEDED");
    }
    assert(threw);
}

TEST(overwrite_existing_path) {
    KeyRegistry reg;
    reg.registerOne(1, "key1", DataType::String);
    reg.registerOne(2, "key1", DataType::Int32); // Same path, different keyId

    // The new keyId should be registered
    assert(reg.hasKeyId(2));
    auto* entry = reg.getByPath("key1");
    assert(entry != nullptr);
    assert(entry->keyId == 2);
}

int main() {
    std::cout << "KeyRegistry tests:\n";
    // Tests run via static initialization

    std::cout << "\n" << tests_passed << " passed, " << tests_failed << " failed\n";
    return tests_failed > 0 ? 1 : 0;
}
