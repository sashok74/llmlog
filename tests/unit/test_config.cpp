// Unit tests for llmlog::core config loader — pure JSON/filesystem logic,
// no Firebird required.

#include <gtest/gtest.h>

#include "llmlog/core/config.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using llmlog::core::Config;
using llmlog::core::ConfigError;
using llmlog::core::loadConfigFromString;

namespace {

class TempDirFixture : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = fs::temp_directory_path() /
               ("llmlog_cfgtest_" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) +
                "_" + ::testing::UnitTest::GetInstance()->current_test_info()->name());
        fs::create_directories(dir_);
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }
    fs::path writeFile(const std::string& name, std::string_view content) const {
        auto p = dir_ / name;
        std::ofstream f(p);
        f << content;
        return p;
    }
    fs::path dir_;
};

// Minimal valid config used as a fixture in multiple tests.
constexpr const char* kMinimalConfig = R"JSON({
  "database": {
    "server":   "localhost",
    "port":     3050,
    "path":     "/var/lib/llmlog/db.fdb",
    "user":     "SYSDBA",
    "password": "literal_pw",
    "charset":  "UTF8"
  },
  "providers": {
    "deepseek": {
      "base_url": "https://api.deepseek.com/v1",
      "api_key":  "sk-literal-deepseek"
    }
  }
})JSON";

} // namespace

// =======================================================================
//  Happy paths
// =======================================================================
TEST(ConfigLoader, LoadsMinimalValidConfig) {
    auto cfg = loadConfigFromString(kMinimalConfig, fs::current_path());
    EXPECT_EQ(cfg.database.server,   "localhost");
    EXPECT_EQ(cfg.database.port,     3050);
    EXPECT_EQ(cfg.database.user,     "SYSDBA");
    EXPECT_EQ(cfg.database.password, "literal_pw");
    ASSERT_TRUE(cfg.providers.count("deepseek"));
    EXPECT_EQ(cfg.providers.at("deepseek").api_key, "sk-literal-deepseek");
    // Defaults propagate where the JSON omits them.
    EXPECT_EQ(cfg.bind.host,     "127.0.0.1");
    EXPECT_EQ(cfg.bind.port,     7788);
    EXPECT_EQ(cfg.logging.level, "info");
}

TEST(ConfigLoader, DefaultAuthHeadersAndScheme) {
    auto cfg = loadConfigFromString(kMinimalConfig, fs::current_path());
    const auto& p = cfg.providers.at("deepseek");
    EXPECT_EQ(p.auth_header, "Authorization");
    EXPECT_EQ(p.auth_scheme, "Bearer");
    EXPECT_TRUE(p.extra_headers.empty());
}

TEST(ConfigLoader, ProviderExtraHeadersPreserved) {
    constexpr const char* kCfg = R"JSON({
      "database": {"path":"x","user":"SYSDBA","password":"pw"},
      "providers": {
        "anthropic": {
          "base_url": "https://api.anthropic.com",
          "api_key":  "sk-ant-x",
          "auth_header": "x-api-key",
          "auth_scheme": "",
          "extra_headers": {"anthropic-version": "2023-06-01", "x-custom": "1"}
        }
      }
    })JSON";
    auto cfg = loadConfigFromString(kCfg, fs::current_path());
    const auto& p = cfg.providers.at("anthropic");
    EXPECT_EQ(p.auth_header, "x-api-key");
    EXPECT_EQ(p.auth_scheme, "");
    EXPECT_EQ(p.extra_headers.at("anthropic-version"), "2023-06-01");
    EXPECT_EQ(p.extra_headers.at("x-custom"),          "1");
}

TEST(ConfigLoader, UnderscorePrefixedProviderKeysAreSkipped) {
    // The checked-in template uses keys like "_anthropic_template" as
    // commented-out examples. They must not appear as real providers.
    constexpr const char* kCfg = R"JSON({
      "database": {"path":"x","user":"SYSDBA","password":"pw"},
      "providers": {
        "_comment": "just a note",
        "_anthropic_template": {"base_url":"x","api_key":"x"},
        "openai": {"base_url": "https://api.openai.com/v1", "api_key": "sk-xxx"}
      }
    })JSON";
    auto cfg = loadConfigFromString(kCfg, fs::current_path());
    EXPECT_EQ(cfg.providers.size(), 1u);
    EXPECT_TRUE(cfg.providers.count("openai"));
    EXPECT_FALSE(cfg.providers.count("_comment"));
    EXPECT_FALSE(cfg.providers.count("_anthropic_template"));
}

// =======================================================================
//  Source resolution
// =======================================================================
TEST_F(TempDirFixture, ResolvesApiKeyFromFile) {
    auto keyFile = writeFile("anthropic.key", "sk-ant-from-file\n");
    // Use generic_string() so Windows path backslashes don't corrupt the
    // JSON literal ('C:\U...' would be parsed as a bad escape sequence).
    const std::string cfgText = R"JSON({
      "database": {"path":"x","user":"SYSDBA","password":"pw"},
      "providers": {
        "anthropic": {
          "base_url": "https://api.anthropic.com",
          "api_key_file": ")JSON" + keyFile.generic_string() + R"JSON("
        }
      }
    })JSON";
    auto cfg = loadConfigFromString(cfgText, dir_);
    // Trailing \n must be stripped.
    EXPECT_EQ(cfg.providers.at("anthropic").api_key, "sk-ant-from-file");
}

TEST_F(TempDirFixture, ResolvesApiKeyFromFileRelativePath) {
    writeFile("rel.key", "sk-rel");
    const std::string cfgText = R"JSON({
      "database": {"path":"x","user":"SYSDBA","password":"pw"},
      "providers": {
        "openai": {
          "base_url": "https://api.openai.com/v1",
          "api_key_file": "rel.key"
        }
      }
    })JSON";
    auto cfg = loadConfigFromString(cfgText, dir_);
    EXPECT_EQ(cfg.providers.at("openai").api_key, "sk-rel");
}

TEST_F(TempDirFixture, ResolvesApiKeyFromEnv) {
#ifdef _WIN32
    _putenv_s("LLMLOG_TEST_KEY_ENV", "sk-from-env");
#else
    setenv("LLMLOG_TEST_KEY_ENV", "sk-from-env", 1);
#endif
    const char* cfgText = R"JSON({
      "database": {"path":"x","user":"SYSDBA","password":"pw"},
      "providers": {
        "deepseek": {
          "base_url": "https://api.deepseek.com/v1",
          "api_key_env": "LLMLOG_TEST_KEY_ENV"
        }
      }
    })JSON";
    auto cfg = loadConfigFromString(cfgText, fs::current_path());
    EXPECT_EQ(cfg.providers.at("deepseek").api_key, "sk-from-env");
#ifdef _WIN32
    _putenv_s("LLMLOG_TEST_KEY_ENV", "");
#else
    unsetenv("LLMLOG_TEST_KEY_ENV");
#endif
}

TEST(ConfigLoader, ResolvesDatabasePasswordFromEnv) {
#ifdef _WIN32
    _putenv_s("LLMLOG_TEST_DB_PW", "secret-db-pw");
#else
    setenv("LLMLOG_TEST_DB_PW", "secret-db-pw", 1);
#endif
    const char* cfgText = R"JSON({
      "database": {
        "path":"x","user":"SYSDBA","password_env":"LLMLOG_TEST_DB_PW"
      },
      "providers": {}
    })JSON";
    auto cfg = loadConfigFromString(cfgText, fs::current_path());
    EXPECT_EQ(cfg.database.password, "secret-db-pw");
#ifdef _WIN32
    _putenv_s("LLMLOG_TEST_DB_PW", "");
#else
    unsetenv("LLMLOG_TEST_DB_PW");
#endif
}

// =======================================================================
//  Error paths
// =======================================================================
TEST(ConfigLoader, MalformedJsonThrows) {
    EXPECT_THROW(loadConfigFromString("not json", fs::current_path()), ConfigError);
    EXPECT_THROW(loadConfigFromString("[]",       fs::current_path()), ConfigError);
}

TEST(ConfigLoader, MissingDatabaseSectionThrows) {
    EXPECT_THROW(loadConfigFromString(R"({"providers":{}})", fs::current_path()),
                 ConfigError);
}

TEST(ConfigLoader, MissingDatabasePathThrowsWithHelpfulPath) {
    try {
        loadConfigFromString(
            R"({"database":{"user":"SYSDBA","password":"pw"},"providers":{}})",
            fs::current_path());
        FAIL() << "expected ConfigError";
    } catch (const ConfigError& e) {
        EXPECT_NE(std::string(e.jsonPath()).find("database"), std::string::npos);
    }
}

TEST(ConfigLoader, ProviderMissingApiKeyAndFileAndEnvThrows) {
    const char* cfgText = R"JSON({
      "database": {"path":"x","user":"SYSDBA","password":"pw"},
      "providers": { "bare": {"base_url": "https://example.com"} }
    })JSON";
    try {
        loadConfigFromString(cfgText, fs::current_path());
        FAIL() << "expected ConfigError";
    } catch (const ConfigError& e) {
        EXPECT_NE(std::string(e.jsonPath()).find("providers/bare"), std::string::npos);
    }
}

TEST(ConfigLoader, ProviderSettingBothApiKeyAndFileThrows) {
    const char* cfgText = R"JSON({
      "database": {"path":"x","user":"SYSDBA","password":"pw"},
      "providers": {
        "dup": {
          "base_url": "https://example.com",
          "api_key": "sk-x",
          "api_key_file": "/nonexistent"
        }
      }
    })JSON";
    EXPECT_THROW(loadConfigFromString(cfgText, fs::current_path()), ConfigError);
}

TEST_F(TempDirFixture, ApiKeyFileRefNotFoundThrows) {
    const char* cfgText = R"JSON({
      "database": {"path":"x","user":"SYSDBA","password":"pw"},
      "providers": {
        "x": {
          "base_url": "https://example.com",
          "api_key_file": "does-not-exist.key"
        }
      }
    })JSON";
    EXPECT_THROW(loadConfigFromString(cfgText, dir_), ConfigError);
}

TEST(ConfigLoader, ApiKeyEnvVarUnsetThrows) {
    // Make sure the var is genuinely not set.
#ifdef _WIN32
    _putenv_s("LLMLOG_MISSING_ENV_VAR_XYZ", "");
#else
    unsetenv("LLMLOG_MISSING_ENV_VAR_XYZ");
#endif
    const char* cfgText = R"JSON({
      "database": {"path":"x","user":"SYSDBA","password":"pw"},
      "providers": {
        "y": {
          "base_url": "https://example.com",
          "api_key_env": "LLMLOG_MISSING_ENV_VAR_XYZ"
        }
      }
    })JSON";
    EXPECT_THROW(loadConfigFromString(cfgText, fs::current_path()), ConfigError);
}

TEST(ConfigLoader, EmptyLiteralApiKeyThrows) {
    const char* cfgText = R"JSON({
      "database": {"path":"x","user":"SYSDBA","password":"pw"},
      "providers": {
        "z": {"base_url":"https://example.com","api_key":""}
      }
    })JSON";
    EXPECT_THROW(loadConfigFromString(cfgText, fs::current_path()), ConfigError);
}
